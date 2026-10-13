#include "mnemonicdb/mnemonic_modify.hpp"
#include "mnemonicdb/mnemonic_table_entry.hpp"
#include "mnemonicdb/mnemonic_transaction.hpp"
#include "mnemonicdb/tempid.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_update.hpp"

namespace duckdb {

using mnemonicdb::AttributeDef;
using mnemonicdb::EntityId;

//! A sink that changes rows identified by their row id and reports how many it changed
class MnemonicModify : public PhysicalOperator {
public:
	MnemonicModify(PhysicalPlan &physical_plan, vector<LogicalType> types, MnemonicTableEntry &table,
	               idx_t row_id_index, RowIdHandling row_id_handling, idx_t estimated_cardinality)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
	      table(table), row_id_index(row_id_index), row_id_handling(row_id_handling) {
	}

	MnemonicTableEntry &table;
	idx_t row_id_index;
	RowIdHandling row_id_handling;

	struct State : public GlobalSinkState {
		idx_t count = 0;
		unordered_set<EntityId> seen;
	};

public:
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &) const override {
		return make_uniq<State>();
	}
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override {
		auto &state = input.global_state.Cast<State>();
		auto &transaction = MnemonicTransaction::Get(context.client, table.catalog);
		auto &writer = transaction.GetWriter();
		chunk.Flatten();
		auto &row_ids = chunk.data[row_id_index];
		for (idx_t row = 0; row < chunk.size(); row++) {
			if (FlatVector::IsNull(row_ids, row)) {
				continue;
			}
			auto entity = FlatVector::GetData<uint64_t>(row_ids)[row];
			if (!state.seen.insert(entity).second) {
				if (row_id_handling == RowIdHandling::ERROR) {
					throw InvalidInputException("MnemonicDB: the same row is modified more than once");
				}
				// The first match wins for UPDATE ... FROM; a repeated DELETE of the same row is a no-op
				continue;
			}
			ModifyRow(writer, chunk, row, entity);
			state.count++;
		}
		return SinkResultType::NEED_MORE_INPUT;
	}
	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return false;
	}
	bool IsSource() const override {
		return true;
	}

protected:
	virtual void ModifyRow(mnemonicdb::TxWriter &writer, DataChunk &chunk, idx_t row, EntityId entity) const = 0;

	SourceResultType GetDataInternal(ExecutionContext &, DataChunk &chunk, OperatorSourceInput &) const override {
		chunk.data[0].Append(Value::BIGINT(NumericCast<int64_t>(sink_state->Cast<State>().count)));
		return SourceResultType::FINISHED;
	}
};

//===--------------------------------------------------------------------===//
// DELETE
//===--------------------------------------------------------------------===//
class MnemonicDelete : public MnemonicModify {
public:
	using MnemonicModify::MnemonicModify;

	string GetName() const override {
		return "MNEMONICDB_DELETE";
	}

protected:
	void ModifyRow(mnemonicdb::TxWriter &writer, DataChunk &, idx_t, EntityId entity) const override {
		for (idx_t a = 1; a <= table.GetTableDef().attributes.size(); a++) {
			writer.RetractAll(table.GetAttribute(a), entity);
		}
	}
};

PhysicalOperator &PlanMnemonicDelete(ClientContext &, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                     PhysicalOperator &plan) {
	if (op.return_chunk) {
		throw NotImplementedException("MnemonicDB does not support DELETE ... RETURNING yet");
	}
	auto &table = op.table.Cast<MnemonicTableEntry>();
	if (table.GetTableDef().schema != "main") {
		throw NotImplementedException("MnemonicDB does not support writing to sys tables yet");
	}
	auto row_id_index = op.expressions.back()->Cast<BoundReferenceExpression>().Index();
	auto &result = planner.Make<MnemonicDelete>(op.types, table, row_id_index, RowIdHandling::KEEP_FIRST,
	                                            op.estimated_cardinality);
	result.children.push_back(plan);
	return result;
}

//===--------------------------------------------------------------------===//
// UPDATE
//===--------------------------------------------------------------------===//
class MnemonicUpdate : public MnemonicModify {
public:
	MnemonicUpdate(PhysicalPlan &physical_plan, vector<LogicalType> types, MnemonicTableEntry &table,
	               idx_t row_id_index, RowIdHandling row_id_handling, idx_t estimated_cardinality,
	               vector<PhysicalIndex> columns_p, vector<idx_t> value_indexes_p)
	    : MnemonicModify(physical_plan, std::move(types), table, row_id_index, row_id_handling,
	                     estimated_cardinality),
	      columns(std::move(columns_p)), value_indexes(std::move(value_indexes_p)) {
	}

	//! Updated physical columns, and where each one's new value is in the input chunk
	vector<PhysicalIndex> columns;
	vector<idx_t> value_indexes;

	string GetName() const override {
		return "MNEMONICDB_UPDATE";
	}

protected:
	void ModifyRow(mnemonicdb::TxWriter &writer, DataChunk &chunk, idx_t row, EntityId entity) const override {
		for (idx_t i = 0; i < columns.size(); i++) {
			auto &attr = table.GetAttribute(columns[i].index);
			auto &vector = chunk.data[value_indexes[i]];
			if (FlatVector::IsNull(vector, row)) {
				writer.RetractAll(attr, entity);
				continue;
			}
			if (!attr.many) {
				writer.Assert(attr, entity, Encode(attr, vector, row));
				continue;
			}
			// The new list replaces the set: retract what is gone, assert what is new
			auto list = FlatVector::GetData<list_entry_t>(vector)[row];
			auto &child = ListVector::GetChild(vector);
			unordered_set<string> wanted;
			for (idx_t c = list.offset; c < list.offset + list.length; c++) {
				if (FlatVector::IsNull(child, c)) {
					throw InvalidInputException("MnemonicDB: cardinality-many column \"%s\" cannot hold NULL elements",
					                            attr.name);
				}
				wanted.insert(Encode(attr, child, c));
			}
			for (auto &datom : writer.CurrentDatoms(entity, attr.Local())) {
				if (wanted.erase(datom.v) == 0) {
					writer.Retract(attr, entity, datom.v);
				}
			}
			for (auto &v : wanted) {
				writer.Assert(attr, entity, v);
			}
		}
	}

private:
	static string Encode(const AttributeDef &attr, const Vector &vector, idx_t row) {
		if (attr.is_ref) {
			auto id = FlatVector::GetData<uint64_t>(vector)[row];
			if (mnemonicdb::IsTempid(id) || !mnemonicdb::IsWellFormed(id)) {
				throw InvalidInputException("MnemonicDB: %d is not a valid entity ID for column \"%s\"", id,
				                            attr.name);
			}
			string v;
			mnemonicdb::PutEntityId(v, id);
			return v;
		}
		return attr.codec.EncodeValue(vector.GetValue(row));
	}
};

PhysicalOperator &PlanMnemonicUpdate(ClientContext &, PhysicalPlanGenerator &planner, LogicalUpdate &op,
                                     PhysicalOperator &plan) {
	if (op.return_chunk || op.capture_old_rows) {
		throw NotImplementedException("MnemonicDB does not support UPDATE ... RETURNING yet");
	}
	auto &table = op.table.Cast<MnemonicTableEntry>();
	if (table.GetTableDef().schema != "main") {
		throw NotImplementedException("MnemonicDB does not support writing to sys tables yet");
	}
	vector<PhysicalIndex> columns;
	vector<idx_t> value_indexes;
	for (idx_t i = 0; i < op.columns.size(); i++) {
		if (op.columns[i].index == MNEMONIC_ID_COLUMN_INDEX) {
			// A delete-and-insert update (e.g. of a LIST column) carries every column, `id` unchanged
			if (op.update_is_del_and_insert) {
				continue;
			}
			throw BinderException("MnemonicDB: the id column cannot be updated");
		}
		columns.push_back(op.columns[i]);
		auto &expr = *op.expressions[i];
		value_indexes.push_back(expr.GetExpressionType() == ExpressionType::BOUND_REF
		                            ? expr.Cast<BoundReferenceExpression>().Index()
		                            : i);
	}
	// Row ids are the last column of the input chunk
	auto row_id_index = plan.types.size() - 1;
	auto &result = planner.Make<MnemonicUpdate>(op.types, table, row_id_index, op.row_id_handling,
	                                            op.estimated_cardinality, std::move(columns), std::move(value_indexes));
	result.children.push_back(plan);
	return result;
}

} // namespace duckdb
