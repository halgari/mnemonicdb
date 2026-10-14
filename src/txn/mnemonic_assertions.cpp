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
#include <functional>

#if defined(_WIN32)
#include "duckdb/common/windows.hpp"
#include <process.h>
#else
#include <pthread.h>
#endif

namespace duckdb {

using mnemonicdb::ColumnFamily;
using mnemonicdb::EntityId;

//! How many offending rows an assertion failure quotes
static constexpr idx_t ASSERTION_ROWS_SHOWN = 5;
//! The setting that says how many assertions are evaluated at once
static constexpr const char *THREADS_SETTING = "mnemonicdb_assertion_threads";
static constexpr int64_t DEFAULT_ASSERTION_THREADS = 4;
static constexpr int64_t MAX_ASSERTION_THREADS = 64;
//! Test hook: how many evaluating threads may be started before starting one
//! fails as if the system refused (negative: no limit)
static constexpr const char *THREAD_LIMIT_SETTING = "mnemonicdb_debug_assertion_thread_limit";
//! Stack of an evaluating thread: a typical main-thread stack, since an
//! assertion's query is planned recursively (macOS defaults threads to 512 KiB,
//! Windows to 1 MiB)
static constexpr idx_t ASSERTION_THREAD_STACK = 8 * 1024 * 1024;

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

//! A thread with an explicit stack size, joined when destroyed. Start returns
//! false when the thread could not be created.
class WorkerThread {
public:
	WorkerThread() = default;
	WorkerThread(const WorkerThread &) = delete;
	WorkerThread &operator=(const WorkerThread &) = delete;
	~WorkerThread() {
		Join();
	}

	bool Start(std::function<void()> body_p) {
		body = std::move(body_p);
#if defined(_WIN32)
		auto result = _beginthreadex(nullptr, static_cast<unsigned>(ASSERTION_THREAD_STACK), &WorkerThread::Run, this,
		                             STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
		if (result == 0) {
			return false;
		}
		handle = reinterpret_cast<HANDLE>(result);
		started = true;
#else
		pthread_attr_t attr;
		if (pthread_attr_init(&attr) != 0) {
			return false;
		}
		bool ok = pthread_attr_setstacksize(&attr, ASSERTION_THREAD_STACK) == 0 &&
		          pthread_create(&thread, &attr, &WorkerThread::Run, this) == 0;
		pthread_attr_destroy(&attr);
		if (!ok) {
			return false;
		}
		started = true;
#endif
		return true;
	}

	void Join() {
		if (!started) {
			return;
		}
		started = false;
#if defined(_WIN32)
		WaitForSingleObject(handle, INFINITE);
		CloseHandle(handle);
#else
		pthread_join(thread, nullptr);
#endif
	}

private:
#if defined(_WIN32)
	static unsigned __stdcall Run(void *self) {
		static_cast<WorkerThread *>(self)->body();
		return 0;
	}
	HANDLE handle = nullptr;
#else
	static void *Run(void *self) {
		static_cast<WorkerThread *>(self)->body();
		return nullptr;
	}
	pthread_t thread;
#endif
	std::function<void()> body;
	bool started = false;
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

unique_ptr<SQLStatement> AssertionStatementCache::Get(const string &key,
                                                     const shared_ptr<CompiledGrammar> &grammar) {
	lock_guard<mutex> guard(lock);
	auto entry = entries.find(key);
	if (entry == entries.end() || entry->second.grammar.get() != grammar.get()) {
		return nullptr;
	}
	return entry->second.statement->Copy();
}

void AssertionStatementCache::Put(const string &key, const SQLStatement &statement,
                                  shared_ptr<CompiledGrammar> grammar) {
	lock_guard<mutex> guard(lock);
	entries[key] = Entry {statement.Copy(), std::move(grammar)};
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

//! What parsing depends on besides the text and the grammar: a parsed query is
//! reused only under the same options. (The extension callback manager is the
//! database's own, so only how many parser extensions it holds can change.)
static string ParserOptionsKey(const ParserOptions &options) {
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
	                      std::to_string(parser_extensions)};
	return StringUtil::Join(parts, " ");
}

//! The assertion's query as one SELECT statement: from the cache, or parsed
//! (and then cached). Returns the reason it is invalid, or an empty string.
static string ParseAssertion(Connection &con, AssertionStatementCache &cache, const string &key,
                             const shared_ptr<CompiledGrammar> &grammar, const AssertionDef &assertion,
                             unique_ptr<SQLStatement> &statement) {
	statement = cache.Get(key, grammar);
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
	cache.Put(key, *statements[0], grammar);
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
                           const shared_ptr<CompiledGrammar> &grammar,
                           const AssertionDef &assertion, bool &aborted) {
	auto quoted = "assertion \"" + assertion.name + "\"";
	if (!assertion.has_name) {
		return quoted + " is invalid: it has no name";
	}
	if (!assertion.has_query) {
		return quoted + " is invalid: it has no query";
	}
	unique_ptr<SQLStatement> statement;
	auto invalid = ParseAssertion(con, cache, key, grammar, assertion, statement);
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
	auto options = con.context->GetParserOptions();
	auto options_key = ParserOptionsKey(options);
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
		run.failures[i] = RunAssertion(con, cache, run.keys[i], options.compiled_grammar, assertion, aborted);
		if (aborted) {
			RunStatement(con, "ROLLBACK");
			open = false;
		}
	}
	if (open) {
		RunStatement(con, "ROLLBACK");
	}
}

//! An integer setting of the committing connection, or `fallback`
static int64_t IntSetting(ClientContext &context, const char *name, int64_t fallback) {
	Value setting;
	if (context.TryGetCurrentSetting(Identifier(name), setting) && !setting.IsNull()) {
		return setting.GetValue<int64_t>();
	}
	return fallback;
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
	// of them, so evaluation goes on with however many others could be started
	auto setting = IntSetting(context, THREADS_SETTING, DEFAULT_ASSERTION_THREADS);
	auto threads = MinValue<idx_t>(idx_t(MinValue(MaxValue<int64_t>(setting, 1), MAX_ASSERTION_THREADS)),
	                               assertions.size());
	auto thread_limit = IntSetting(context, THREAD_LIMIT_SETTING, -1);
	vector<std::exception_ptr> errors(threads);
	auto evaluate = [&run, &errors](idx_t worker) {
		try {
			RunAssertions(run);
		} catch (...) {
			errors[worker] = std::current_exception();
		}
	};
	{
		// Declared after `errors` and `run`: every started thread is joined
		// before they go, on every path
		vector<unique_ptr<WorkerThread>> workers;
		for (idx_t worker = 1; worker < threads; worker++) {
			if (thread_limit >= 0 && idx_t(thread_limit) < worker) {
				break;
			}
			auto thread = make_uniq<WorkerThread>();
			bool started = false;
			try {
				started = thread->Start([&evaluate, worker]() { evaluate(worker); });
			} catch (...) {
				// std::function or allocation failure: as if not started
			}
			if (!started) {
				break;
			}
			workers.push_back(std::move(thread));
		}
		evaluate(0);
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
	                          "How many commit assertions are evaluated at once, each on its own connection (1 to 64)",
	                          LogicalType::BIGINT, Value::BIGINT(DEFAULT_ASSERTION_THREADS));
	config.AddExtensionOption(Identifier(THREAD_LIMIT_SETTING),
	                          "Testing only: how many threads may be started to evaluate commit assertions before "
	                          "starting one fails as if the system refused (negative: no limit)",
	                          LogicalType::BIGINT, Value::BIGINT(-1));
}

} // namespace duckdb
