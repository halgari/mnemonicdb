#include "mnemonicdb/mnemonic_assertions.hpp"
#include "mnemonicdb/mnemonic_catalog.hpp"
#include "mnemonicdb/mnemonic_transaction.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/sql_statement.hpp"

#include "duckdb/main/extension_callback_manager.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/parser/parser_options.hpp"

#include <atomic>
#include <cstdint>
#include <exception>
#include <map>
#include <set>
#include <thread>

namespace duckdb {

using mnemonicdb::ColumnFamily;
using mnemonicdb::EntityId;

//! How many offending rows an assertion failure quotes
static constexpr idx_t ASSERTION_ROWS_SHOWN = 5;
//! The setting that says how many assertions are evaluated at once
static constexpr const char *THREADS_SETTING = "mnemonicdb_assertion_threads";
static constexpr int64_t DEFAULT_ASSERTION_THREADS = 4;

namespace {

struct AssertionDef {
	string name;
	string query;
	bool has_name = false;
	bool has_query = false;
};

//! Registers a connection's context as evaluating assertions for `parent`, so
//! the transactions it starts on this database read the parent's pending state
class AssertionScope {
public:
	AssertionScope(MnemonicCatalog &catalog_p, ClientContext &context_p, MnemonicTransaction &parent)
	    : catalog(catalog_p), context(context_p) {
		catalog.RegisterAssertionContext(context, parent);
	}
	~AssertionScope() {
		catalog.UnregisterAssertionContext(context);
	}

private:
	MnemonicCatalog &catalog;
	ClientContext &context;
};

//! One evaluation of the assertions: the work the evaluating connections share
struct AssertionRun {
	AssertionRun(MnemonicCatalog &catalog_p, MnemonicTransaction &parent_p, DatabaseInstance &db_p,
	             const vector<AssertionDef> &assertions_p)
	    : catalog(catalog_p), parent(parent_p), db(db_p), assertions(assertions_p), failures(assertions_p.size()),
	      keys(assertions_p.size()) {
	}

	MnemonicCatalog &catalog;
	MnemonicTransaction &parent;
	DatabaseInstance &db;
	const vector<AssertionDef> &assertions;
	//! The next assertion to evaluate
	std::atomic<idx_t> next {0};
	//! Per assertion: why it failed (empty if it passed), and its parse cache key
	vector<string> failures;
	vector<string> keys;
};

} // namespace

AssertionStatementCache::AssertionStatementCache() {
}

AssertionStatementCache::~AssertionStatementCache() {
}

unique_ptr<SQLStatement> AssertionStatementCache::Get(const string &key) {
	lock_guard<mutex> guard(lock);
	auto entry = entries.find(key);
	return entry == entries.end() ? nullptr : entry->second->Copy();
}

void AssertionStatementCache::Put(const string &key, const SQLStatement &statement) {
	lock_guard<mutex> guard(lock);
	entries[key] = statement.Copy();
}

void AssertionStatementCache::Retain(const vector<string> &keys) {
	lock_guard<mutex> guard(lock);
	std::set<string> keep(keys.begin(), keys.end());
	for (auto it = entries.begin(); it != entries.end();) {
		if (keep.count(it->first)) {
			++it;
		} else {
			it = entries.erase(it);
		}
	}
}

//! The assertions visible to the transaction, including its own changes to
//! sys.assertions, ordered by name
static vector<AssertionDef> LoadAssertions(mnemonicdb::ReadView &view, const mnemonicdb::Schema &schema) {
	std::map<EntityId, AssertionDef> by_entity;
	auto &name_attr = schema.GetAttribute(mnemonicdb::sys::ASSERTIONS_NAME);
	for (auto &datom : view.ScanPrefix(ColumnFamily::AEVT, mnemonicdb::AevtPrefix(name_attr.Local()))) {
		auto &def = by_entity[datom.e];
		def.name = name_attr.codec.DecodeValue(datom.v).ToString();
		def.has_name = true;
	}
	auto &query_attr = schema.GetAttribute(mnemonicdb::sys::ASSERTIONS_QUERY);
	for (auto &datom : view.ScanPrefix(ColumnFamily::AEVT, mnemonicdb::AevtPrefix(query_attr.Local()))) {
		auto &def = by_entity[datom.e];
		def.query = query_attr.codec.DecodeValue(datom.v).ToString();
		def.has_query = true;
	}
	vector<AssertionDef> result;
	for (auto &entry : by_entity) {
		auto &def = entry.second;
		if (!def.has_name) {
			// Rows in sys.assertions need a name: report it under its entity id
			def.name = "#" + std::to_string(entry.first);
		}
		result.push_back(std::move(def));
	}
	std::sort(result.begin(), result.end(),
	          [](const AssertionDef &a, const AssertionDef &b) { return a.name < b.name; });
	return result;
}

static string RenderRow(DataChunk &chunk, idx_t row) {
	vector<string> values;
	for (idx_t col = 0; col < chunk.ColumnCount(); col++) {
		values.push_back(chunk.GetValue(col, row).ToString());
	}
	return "(" + StringUtil::Join(values, ", ") + ")";
}

//! What parsing depends on besides the text: a parsed query is reused only
//! under the same options
static string ParserOptionsKey(ClientContext &context) {
	auto options = context.GetParserOptions();
	idx_t parser_extensions = 0;
	if (options.extensions) {
		for (auto &extension : options.extensions->ParserExtensions()) {
			(void)extension;
			parser_extensions++;
		}
	}
	vector<string> parts {std::to_string(static_cast<int>(options.identifier_case_mode)),
	                      std::to_string(options.integer_division),
	                      std::to_string(options.heap_based_parser),
	                      std::to_string(static_cast<int>(options.regex_match_operator_semantics)),
	                      std::to_string(options.max_expression_depth),
	                      std::to_string(static_cast<int>(options.parser_override_setting)),
	                      std::to_string(reinterpret_cast<uintptr_t>(options.compiled_grammar.get())),
	                      std::to_string(reinterpret_cast<uintptr_t>(options.extensions.get())),
	                      std::to_string(parser_extensions)};
	return StringUtil::Join(parts, " ");
}

//! The assertion's query as one SELECT statement: from the cache, or parsed
//! (and then cached). Returns the reason it is invalid, or an empty string.
static string ParseAssertion(Connection &con, AssertionStatementCache &cache, const string &key,
                             const AssertionDef &assertion, unique_ptr<SQLStatement> &statement) {
	statement = cache.Get(key);
	if (statement) {
		return string();
	}
	vector<unique_ptr<SQLStatement>> statements;
	try {
		statements = con.ExtractStatements(assertion.query);
	} catch (std::exception &ex) {
		return ErrorData(ex).Message();
	}
	if (statements.size() != 1 || statements[0]->type != StatementType::SELECT_STATEMENT) {
		return "its query must be a single SELECT statement";
	}
	cache.Put(key, *statements[0]);
	statement = std::move(statements[0]);
	return string();
}

static void RunStatement(Connection &con, const string &query) {
	auto result = con.Query(query);
	if (result->HasError()) {
		result->ThrowError();
	}
}

//! Run one assertion in the connection's open read-only transaction; returns
//! the failure message, or an empty string if it passed. Sets `aborted` when
//! the query failed, which leaves the transaction unusable.
static string RunAssertion(Connection &con, AssertionStatementCache &cache, const string &key,
                           const AssertionDef &assertion, bool &aborted) {
	auto quoted = "assertion \"" + assertion.name + "\"";
	if (!assertion.has_name) {
		return quoted + " is invalid: it has no name";
	}
	if (!assertion.has_query) {
		return quoted + " is invalid: it has no query";
	}
	unique_ptr<SQLStatement> statement;
	auto invalid = ParseAssertion(con, cache, key, assertion, statement);
	if (!invalid.empty()) {
		return quoted + " is invalid: " + invalid;
	}

	auto result = con.SendQuery(std::move(statement));
	idx_t row_count = 0;
	vector<string> shown;
	if (!result->HasError()) {
		while (true) {
			auto chunk = result->Fetch();
			if (!chunk || chunk->size() == 0) {
				break;
			}
			for (idx_t row = 0; row < chunk->size() && shown.size() < ASSERTION_ROWS_SHOWN; row++) {
				shown.push_back(RenderRow(*chunk, row));
			}
			row_count += chunk->size();
		}
	}
	if (result->HasError()) {
		aborted = true;
		return quoted + " could not run: " + result->GetError();
	}
	if (row_count > 0) {
		return StringUtil::Format("%s failed (%llu row%s): %s%s", quoted, row_count, row_count == 1 ? "" : "s",
		                          StringUtil::Join(shown, ", "), row_count > shown.size() ? ", ..." : "");
	}
	return string();
}

//! One evaluating connection: takes assertions from `run` until none are left,
//! all in one read-only transaction (a new one after a query fails)
static void RunAssertions(AssertionRun &run) {
	// A separate connection: the committing context is locked while it commits.
	// Its transactions on this database are read-only and borrow the committing
	// transaction's snapshot, write batch and schema, so they never take the
	// writer lock.
	Connection con(run.db);
	AssertionScope scope(run.catalog, *con.context, run.parent);
	// Unqualified names in an assertion refer to this database, whatever it is
	// attached as
	RunStatement(con, "USE " + KeywordHelper::WriteOptionallyQuoted(run.catalog.GetName().GetIdentifierName()));
	auto options_key = ParserOptionsKey(*con.context);
	auto &cache = run.catalog.GetAssertionCache();
	bool open = false;
	for (idx_t i = run.next++; i < run.assertions.size(); i = run.next++) {
		auto &assertion = run.assertions[i];
		run.keys[i] = options_key + "\n" + assertion.query;
		if (!open) {
			// Read-only: the query cannot write to this database (its transaction
			// refuses to) nor to any other
			RunStatement(con, "BEGIN TRANSACTION READ ONLY");
			open = true;
		}
		bool aborted = false;
		run.failures[i] = RunAssertion(con, cache, run.keys[i], assertion, aborted);
		if (aborted) {
			RunStatement(con, "ROLLBACK");
			open = false;
		}
	}
	if (open) {
		RunStatement(con, "ROLLBACK");
	}
}

//! How many connections evaluate assertions at once
static idx_t AssertionThreads(ClientContext &context) {
	Value setting;
	if (context.TryGetCurrentSetting(Identifier(THREADS_SETTING), setting) && !setting.IsNull()) {
		return idx_t(MaxValue<int64_t>(setting.GetValue<int64_t>(), 1));
	}
	return idx_t(DEFAULT_ASSERTION_THREADS);
}

void EvaluateAssertions(ClientContext &context, MnemonicTransaction &transaction) {
	auto &view = transaction.GetReadView();
	auto assertions = LoadAssertions(view, transaction.GetSchema());
	if (assertions.empty()) {
		return;
	}
	auto &catalog = transaction.GetCatalog();
	AssertionRun run(catalog, transaction, *context.db, assertions);
	// Each connection evaluates whichever assertion is next; this thread is one
	// of them
	auto threads = MinValue<idx_t>(AssertionThreads(context), assertions.size());
	vector<std::exception_ptr> errors(threads);
	auto evaluate = [&run, &errors](idx_t worker) {
		try {
			RunAssertions(run);
		} catch (...) {
			errors[worker] = std::current_exception();
		}
	};
	vector<std::thread> workers;
	for (idx_t worker = 1; worker < threads; worker++) {
		workers.emplace_back(evaluate, worker);
	}
	evaluate(0);
	for (auto &worker : workers) {
		worker.join();
	}
	for (auto &error : errors) {
		if (error) {
			std::rethrow_exception(error);
		}
	}
	// Only the current assertions' parsed queries are kept
	catalog.GetAssertionCache().Retain(run.keys);

	vector<string> failures;
	for (auto &failure : run.failures) {
		if (!failure.empty()) {
			failures.push_back(std::move(failure));
		}
	}
	if (!failures.empty()) {
		throw ConstraintException(StringUtil::Join(failures, "; "));
	}
}

void RegisterAssertionSettings(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption(Identifier(THREADS_SETTING),
	                          "How many commit assertions are evaluated at once, each on its own connection",
	                          LogicalType::BIGINT, Value::BIGINT(DEFAULT_ASSERTION_THREADS));
}

} // namespace duckdb
