#include "mnemonicdb/mnemonic_assertions.hpp"
#include "mnemonicdb/mnemonic_catalog.hpp"
#include "mnemonicdb/mnemonic_transaction.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/sql_statement.hpp"

#include <map>

namespace duckdb {

using mnemonicdb::ColumnFamily;
using mnemonicdb::EntityId;

//! How many offending rows an assertion failure quotes
static constexpr idx_t ASSERTION_ROWS_SHOWN = 5;

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

} // namespace

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

//! Run one assertion; returns the failure message, or an empty string if it
//! passed
static string RunAssertion(Connection &con, const AssertionDef &assertion) {
	auto quoted = "assertion \"" + assertion.name + "\"";
	if (!assertion.has_name) {
		return quoted + " is invalid: it has no name";
	}
	if (!assertion.has_query) {
		return quoted + " is invalid: it has no query";
	}
	vector<unique_ptr<SQLStatement>> statements;
	try {
		statements = con.ExtractStatements(assertion.query);
	} catch (std::exception &ex) {
		return quoted + " is invalid: " + ErrorData(ex).Message();
	}
	if (statements.size() != 1 || statements[0]->type != StatementType::SELECT_STATEMENT) {
		return quoted + " is invalid: its query must be a single SELECT statement";
	}

	// A read-only transaction: the query cannot write to this database (its
	// transaction refuses to) nor to any other
	auto begin = con.Query("BEGIN TRANSACTION READ ONLY");
	if (begin->HasError()) {
		begin->ThrowError();
	}
	string failure;
	auto result = con.SendQuery(std::move(statements[0]));
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
		failure = quoted + " could not run: " + result->GetError();
	} else if (row_count > 0) {
		failure = StringUtil::Format("%s failed (%llu row%s): %s%s", quoted, row_count, row_count == 1 ? "" : "s",
		                             StringUtil::Join(shown, ", "), row_count > shown.size() ? ", ..." : "");
	}
	result.reset();
	auto rollback = con.Query("ROLLBACK");
	if (rollback->HasError()) {
		rollback->ThrowError();
	}
	return failure;
}

void EvaluateAssertions(ClientContext &context, MnemonicTransaction &transaction) {
	auto &view = transaction.GetReadView();
	auto assertions = LoadAssertions(view, transaction.GetSchema());
	if (assertions.empty()) {
		return;
	}
	auto &catalog = transaction.GetCatalog();
	// A separate connection: the committing context is locked while it commits.
	// Its transactions on this database are read-only and borrow the committing
	// transaction's snapshot, write batch and schema, so they never take the
	// writer lock.
	Connection con(*context.db);
	AssertionScope scope(catalog, *con.context, transaction);
	// Unqualified names in an assertion refer to this database, whatever it is
	// attached as
	auto use = con.Query("USE " + KeywordHelper::WriteOptionallyQuoted(catalog.GetName().GetIdentifierName()));
	if (use->HasError()) {
		use->ThrowError();
	}
	vector<string> failures;
	for (auto &assertion : assertions) {
		auto failure = RunAssertion(con, assertion);
		if (!failure.empty()) {
			failures.push_back(std::move(failure));
		}
	}
	if (!failures.empty()) {
		throw ConstraintException(StringUtil::Join(failures, "; "));
	}
}

} // namespace duckdb
