#include "mnemonicdb/mnemonic_insert.hpp"
#include "mnemonicdb/mnemonic_table_entry.hpp"
#include "mnemonicdb/mnemonic_transaction.hpp"
#include "mnemonicdb/tempid.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"

namespace duckdb {

using mnemonicdb::AttributeDef;
using mnemonicdb::EntityId;

class MnemonicInsert : public PhysicalOperator {
public:
	MnemonicInsert(PhysicalPlan &physical_plan, vector<LogicalType> types, MnemonicTableEntry &table,
	               idx_t estimated_cardinality)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
	      table(table) {
	}

	MnemonicTableEntry &table;

public:
	string GetName() const override {
		return "MNEMONICDB_INSERT";
	}

	// Sink interface: rows are buffered and written in Finalize, so the statement
	// never reads its own writes
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;
	//! Write one chunk of rows; returns how many rows had any value
	idx_t WriteChunk(ClientContext &context, DataChunk &chunk) const;
	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return false;
	}

	// Source interface
	bool IsSource() const override {
		return true;
	}

protected:
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;
};

struct MnemonicInsertState : public GlobalSinkState {
	MnemonicInsertState(ClientContext &context, const vector<LogicalType> &types) : rows(context, types) {
	}
	ColumnDataCollection rows;
	idx_t count = 0;
};

unique_ptr<GlobalSinkState> MnemonicInsert::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<MnemonicInsertState>(context, children[0].get().GetTypes());
}

//! Resolves entity IDs for inserted rows and REF values
class EntityResolver {
public:
	EntityResolver(MnemonicTransaction &transaction, uint8_t default_partition)
	    : transaction(transaction), default_partition(default_partition) {
	}

	//! A REF value: tempids resolve to their bound entity, or to a fresh one
	EntityId ResolveRef(EntityId value) {
		if (!mnemonicdb::IsTempid(value)) {
			Validate(value);
			Reserve(value);
			return value;
		}
		if (auto bound = transaction.FindTempid(value)) {
			return *bound;
		}
		auto id = Allocate(mnemonicdb::TempidTargetPartition(value));
		transaction.BindTempid(value, id);
		return id;
	}

	//! A row's `id`: NULL allocates, a tempid may resolve to an existing entity
	//! through a unique-identity value
	EntityId ResolveRow(optional_ptr<const EntityId> value, optional_ptr<const EntityId> identity_match) {
		if (!value) {
			// A NULL id is a new entity; an identity match then fails the uniqueness
			// check
			return Allocate(default_partition);
		}
		auto id = *value;
		if (!mnemonicdb::IsTempid(id)) {
			Validate(id);
			Reserve(id);
			return id;
		}
		auto bound = transaction.FindTempid(id);
		if (bound && identity_match && *bound != *identity_match) {
			throw ConstraintException("MnemonicDB: tempid resolved to entity %d earlier in this "
			                          "transaction, but "
			                          "its unique identity value belongs to entity %d",
			                          *bound, *identity_match);
		}
		if (bound) {
			return *bound;
		}
		auto resolved = identity_match ? *identity_match : Allocate(mnemonicdb::TempidTargetPartition(id));
		transaction.BindTempid(id, resolved);
		return resolved;
	}

private:
	EntityId Allocate(uint8_t partition) {
		if (partition == mnemonicdb::partition::DB) {
			partition = default_partition;
		}
		if (partition == mnemonicdb::partition::ATTR || partition == mnemonicdb::partition::TABLE) {
			auto id = transaction.GetWriter().GetView().GetStore().AllocateId(partition);
			transaction.GetWriter().MarkFresh(id);
			return id;
		}
		if (partition < mnemonicdb::partition::DB ||
		    (partition >= mnemonicdb::partition::FIRST_RESERVED && partition < mnemonicdb::partition::FIRST_USER)) {
			throw InvalidInputException("MnemonicDB: cannot allocate entity IDs in reserved partition %d", partition);
		}
		auto id = transaction.GetWriter().GetView().GetStore().AllocateId(partition);
		transaction.GetWriter().MarkFresh(id);
		return id;
	}

	void Reserve(EntityId id) {
		transaction.GetWriter().GetView().GetStore().CheckAllocated(id);
	}

	static void Validate(EntityId id) {
		if (!mnemonicdb::IsWellFormed(id)) {
			throw InvalidInputException("MnemonicDB: %d is not a valid entity ID", id);
		}
		auto part = mnemonicdb::PartitionOf(id);
		if ((part >= mnemonicdb::partition::FIRST_RESERVED && part < mnemonicdb::partition::FIRST_USER) ||
		    part == mnemonicdb::partition::TEMP) {
			throw InvalidInputException("MnemonicDB: entity %d is in reserved partition %d", id, part);
		}
	}

	MnemonicTransaction &transaction;
	uint8_t default_partition;
};

//! The partition new rows of a table are allocated in
static uint8_t DefaultPartition(const MnemonicTableEntry &table) {
	auto id = table.GetTableDef().id;
	if (id == mnemonicdb::sys::TABLES) {
		return mnemonicdb::partition::TABLE;
	}
	if (id == mnemonicdb::sys::ATTRIBUTES) {
		return mnemonicdb::partition::ATTR;
	}
	return mnemonicdb::partition::DB;
}

//! Rows of sys tables may only describe user entities (and, for sys.tx, the
//! current transaction)
static void CheckSysEntity(MnemonicTransaction &transaction, const MnemonicTableEntry &table, EntityId entity) {
	auto id = table.GetTableDef().id;
	// sys.assertions rows are ordinary entities, like rows of user tables
	if (table.GetTableDef().schema != "sys" || id == mnemonicdb::sys::ASSERTIONS) {
		if (mnemonicdb::PartitionOf(entity) < mnemonicdb::partition::DB) {
			throw InvalidInputException("MnemonicDB: entity %d is in a system partition", entity);
		}
		return;
	}
	if (id == mnemonicdb::sys::TX) {
		if (entity != transaction.CurrentTxEntity()) {
			throw InvalidInputException("MnemonicDB: rows of sys.tx can only "
			                            "describe the current transaction; use "
			                            "current_tx() as the id");
		}
		return;
	}
	auto expected = DefaultPartition(table);
	auto first_user = expected == mnemonicdb::partition::TABLE ? 16 : 64;
	if (mnemonicdb::PartitionOf(entity) != expected || mnemonicdb::LocalOf(entity) < uint64_t(first_user)) {
		throw InvalidInputException("MnemonicDB: entity %d cannot be described in %s", entity, table.name);
	}
}

SinkResultType MnemonicInsert::Sink(ExecutionContext &, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &state = input.global_state.Cast<MnemonicInsertState>();
	state.rows.Append(chunk);
	return SinkResultType::NEED_MORE_INPUT;
}

SinkFinalizeType MnemonicInsert::Finalize(Pipeline &, Event &, ClientContext &context,
                                          OperatorSinkFinalizeInput &input) const {
	auto &state = input.global_state.Cast<MnemonicInsertState>();
	auto &transaction = MnemonicTransaction::Get(context, table.catalog);
	lock_guard<mutex> guard(transaction.GetWriteLock());
	MnemonicStatement statement(transaction);
	state.count = 0;
	for (auto &chunk : state.rows.Chunks()) {
		state.count += WriteChunk(context, chunk);
	}
	if (table.GetTableDef().schema == "sys" && table.GetTableDef().id != mnemonicdb::sys::TX &&
	    table.GetTableDef().id != mnemonicdb::sys::ASSERTIONS) {
		transaction.ReloadSchema(context);
	}
	statement.Commit();
	return SinkFinalizeType::READY;
}

idx_t MnemonicInsert::WriteChunk(ClientContext &context, DataChunk &chunk) const {
	auto &transaction = MnemonicTransaction::Get(context, table.catalog);
	auto &writer = transaction.GetWriter();
	auto &def = table.GetTableDef();
	auto count = chunk.size();
	chunk.Flatten();

	// Encode every non-REF attribute column once per chunk; cardinality-many
	// columns encode their list children
	idx_t attr_count = def.attributes.size();
	vector<vector<string>> encoded(attr_count);
	for (idx_t a = 0; a < attr_count; a++) {
		auto &attr = table.GetAttribute(a + 1);
		auto &column = chunk.data[a + 1];
		if (attr.is_ref) {
			continue;
		}
		if (attr.many) {
			auto &child = ListVector::GetChild(column);
			attr.codec.EncodeVector(child, ListVector::GetListSize(column), encoded[a]);
		} else {
			attr.codec.EncodeVector(column, count, encoded[a]);
		}
	}

	EntityResolver resolver(transaction, DefaultPartition(table));
	auto ids = chunk.data[MNEMONIC_ID_COLUMN_INDEX].Values<uint64_t>();
	idx_t written = 0;
	for (idx_t row = 0; row < count; row++) {
		// A row without any value asserts nothing: it neither creates an entity nor
		// counts as inserted
		bool has_value = false;
		for (idx_t a = 0; a < attr_count && !has_value; a++) {
			has_value = !FlatVector::IsNull(chunk.data[a + 1], row);
		}
		if (!has_value) {
			continue;
		}
		written++;
		// A unique-identity value already present on an entity identifies the row's
		// entity
		optional_ptr<const EntityId> identity_match;
		EntityId match = 0;
		for (idx_t a = 0; a < attr_count && !identity_match; a++) {
			auto &attr = table.GetAttribute(a + 1);
			if (attr.unique != mnemonicdb::Uniqueness::IDENTITY || attr.is_ref ||
			    FlatVector::IsNull(chunk.data[a + 1], row)) {
				continue;
			}
			auto existing = writer.GetView().ScanPrefix(mnemonicdb::ColumnFamily::AVET,
			                                            mnemonicdb::AvetPrefix(attr.Local(), encoded[a][row]));
			for (auto &datom : existing) {
				if (datom.v == encoded[a][row]) {
					match = datom.e;
					identity_match = &match;
				}
			}
		}
		EntityId row_id_value = 0;
		optional_ptr<const EntityId> row_id;
		if (ids[row].IsValid()) {
			row_id_value = ids[row].GetValue();
			row_id = &row_id_value;
		}
		auto entity = resolver.ResolveRow(row_id, identity_match);
		CheckSysEntity(transaction, table, entity);

		for (idx_t a = 0; a < attr_count; a++) {
			auto &attr = table.GetAttribute(a + 1);
			auto &column = chunk.data[a + 1];
			if (FlatVector::IsNull(column, row)) {
				continue;
			}
			if (!attr.many) {
				if (attr.is_ref) {
					string v;
					mnemonicdb::PutEntityId(v, resolver.ResolveRef(FlatVector::GetData<uint64_t>(column)[row]));
					writer.Assert(attr, entity, v);
				} else {
					writer.Assert(attr, entity, encoded[a][row]);
				}
				continue;
			}
			auto list = FlatVector::GetData<list_entry_t>(column)[row];
			auto &child = ListVector::GetChild(column);
			for (idx_t i = list.offset; i < list.offset + list.length; i++) {
				if (FlatVector::IsNull(child, i)) {
					throw InvalidInputException("MnemonicDB: cardinality-many column "
					                            "\"%s\" cannot hold NULL elements",
					                            attr.name);
				}
				if (attr.is_ref) {
					string v;
					mnemonicdb::PutEntityId(v, resolver.ResolveRef(FlatVector::GetData<uint64_t>(child)[i]));
					writer.Assert(attr, entity, v);
				} else {
					writer.Assert(attr, entity, encoded[a][i]);
				}
			}
		}
	}
	return written;
}

SourceResultType MnemonicInsert::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                 OperatorSourceInput &) const {
	auto &state = sink_state->Cast<MnemonicInsertState>();
	chunk.data[0].Append(Value::BIGINT(NumericCast<int64_t>(state.count)));
	return SourceResultType::FINISHED;
}

PhysicalOperator &PlanMnemonicInsert(ClientContext &, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                     optional_ptr<PhysicalOperator> plan) {
	if (op.return_chunk) {
		throw NotImplementedException("MnemonicDB does not support INSERT ... RETURNING yet");
	}
	if (!plan) {
		throw NotImplementedException("MnemonicDB does not support INSERT without a source");
	}
	auto &table = op.table.Cast<MnemonicTableEntry>();
	table.CheckDmlAllowed();
	auto &insert = planner.Make<MnemonicInsert>(op.types, table, op.estimated_cardinality);
	insert.children.push_back(*plan);
	return insert;
}

} // namespace duckdb
