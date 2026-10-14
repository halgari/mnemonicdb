#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

class ClientContext;
class MnemonicTransaction;
class SQLStatement;

//! Evaluate every assertion in sys.assertions against the pending state of a
//! committing transaction (its own writes included). Throws a
//! ConstraintException naming each failing assertion, with its first rows, if
//! any assertion returns rows or cannot run.
void EvaluateAssertions(ClientContext &context, MnemonicTransaction &transaction);

//! Registers the `mnemonicdb_assertion_threads` setting: how many assertions a
//! commit evaluates at once
void RegisterAssertionSettings(ExtensionLoader &loader);

//! The parsed queries of the assertions, kept across commits (one per database).
//! Parsing is a large share of what an assertion costs (DuckDB's PEG parser takes
//! milliseconds for a query of a few kilobytes), and the queries rarely change.
class AssertionStatementCache {
public:
	AssertionStatementCache();
	~AssertionStatementCache();

	//! A copy of the statement parsed for `key`, or nullptr
	unique_ptr<SQLStatement> Get(const string &key);
	//! Keep a copy of `statement` for `key`
	void Put(const string &key, const SQLStatement &statement);
	//! Drop every entry but those in `keys` (the assertions of the last commit)
	void Retain(const vector<string> &keys);

private:
	mutex lock;
	unordered_map<string, unique_ptr<SQLStatement>> entries;
};

} // namespace duckdb
