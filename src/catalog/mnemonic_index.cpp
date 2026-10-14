#include "mnemonicdb/mnemonic_index.hpp"
#include "mnemonicdb/mnemonic_table_entry.hpp"
#include "mnemonicdb/mnemonic_transaction.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"

namespace duckdb {

MnemonicIndexEntry::MnemonicIndexEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateIndexInfo &info,
                                       Identifier table_name_p)
    : IndexCatalogEntry(catalog, schema, info), table_name(std::move(table_name_p)) {
}

Identifier MnemonicIndexEntry::GetSchemaName() const {
	return schema.name;
}

Identifier MnemonicIndexEntry::GetTableName() const {
	return table_name;
}

unique_ptr<CreateIndexInfo> MakeIndexInfo(MnemonicTableEntry &table, const string &index_name, const string &column) {
	auto info = make_uniq<CreateIndexInfo>();
	info->SetQualification(table.catalog.GetName(), table.schema.name);
	info->SetIndexName(Identifier(index_name));
	info->table = table.name;
	info->index_type = "AVET";
	info->constraint_type = IndexConstraintType::NONE;
	info->expressions.push_back(make_uniq<ColumnRefExpression>(Identifier(column)));
	info->parsed_expressions.push_back(make_uniq<ColumnRefExpression>(Identifier(column)));
	return info;
}

//===--------------------------------------------------------------------===//
// CREATE INDEX operators
//===--------------------------------------------------------------------===//
namespace {

class PhysicalMnemonicCreateIndex : public PhysicalOperator {
public:
	PhysicalMnemonicCreateIndex(PhysicalPlan &physical_plan, MnemonicTableEntry &table, string index_name,
	                            string column, bool if_not_exists)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, {LogicalType::BIGINT}, 1), table(table),
	      index_name(std::move(index_name)), column(std::move(column)), if_not_exists(if_not_exists) {
	}

	MnemonicTableEntry &table;
	string index_name;
	string column;
	bool if_not_exists;

	string GetName() const override {
		return "MNEMONICDB_CREATE_INDEX";
	}
	bool IsSource() const override {
		return true;
	}

protected:
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &, OperatorSourceInput &) const override {
		auto &transaction = MnemonicTransaction::Get(context.client, table.catalog);
		transaction.CreateIndex(table, index_name, column, if_not_exists);
		return SourceResultType::FINISHED;
	}
};

class LogicalMnemonicCreateIndex : public LogicalExtensionOperator {
public:
	LogicalMnemonicCreateIndex(MnemonicTableEntry &table, string index_name, string column, bool if_not_exists)
	    : table(table), index_name(std::move(index_name)), column(std::move(column)), if_not_exists(if_not_exists) {
		types.push_back(LogicalType::BIGINT);
	}

	MnemonicTableEntry &table;
	string index_name;
	string column;
	bool if_not_exists;

	PhysicalOperator &CreatePlan(ClientContext &, PhysicalPlanGenerator &planner) override {
		return planner.Make<PhysicalMnemonicCreateIndex>(table, index_name, column, if_not_exists);
	}
	string GetExtensionName() const override {
		return "mnemonicdb";
	}

protected:
	void ResolveTypes() override {
	}
};

} // namespace

unique_ptr<LogicalOperator> BindMnemonicCreateIndex(MnemonicTableEntry &table, unique_ptr<CreateIndexInfo> info) {
	if (info->constraint_type != IndexConstraintType::NONE) {
		throw BinderException("MnemonicDB: use a UNIQUE or PRIMARY KEY column "
		                      "constraint instead of a unique index");
	}
	if (!info->options.empty()) {
		throw BinderException("MnemonicDB: CREATE INDEX does not take options");
	}
	auto &expressions = info->parsed_expressions.empty() ? info->expressions : info->parsed_expressions;
	if (expressions.size() != 1 || expressions[0]->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		throw BinderException("MnemonicDB: an index covers exactly one column");
	}
	auto &column_ref = expressions[0]->Cast<ColumnRefExpression>();
	auto column = column_ref.GetColumnName().GetIdentifierName();
	if (!table.ColumnExists(Identifier(column))) {
		throw BinderException("MnemonicDB: table \"%s\" has no column \"%s\"", table.name, column);
	}
	auto if_not_exists = info->on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT;
	return make_uniq<LogicalMnemonicCreateIndex>(table, info->GetIndexName().GetIdentifierName(), column,
	                                             if_not_exists);
}

} // namespace duckdb
