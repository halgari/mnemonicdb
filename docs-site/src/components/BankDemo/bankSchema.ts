// Bank Demo Schema Setup

// Split into separate transaction blocks that must be executed as units
export const SCHEMA_BLOCKS = [
  // Block 1: Create account and transaction attributes
  `CALL mnemonic_begin_tx();
INSERT INTO mnemonic_defined_attributes (ident, value_type, cardinality)
VALUES ('account/name', 'db.type/text', 'db.cardinality/one');
INSERT INTO mnemonic_defined_attributes (ident, value_type, cardinality)
VALUES ('account/balance', 'db.type/int8', 'db.cardinality/one');
INSERT INTO mnemonic_defined_attributes (ident, value_type, cardinality, doc)
VALUES ('account/last-seen', 'db.type/int8', 'db.cardinality/one', 'Last step this account had activity');
INSERT INTO mnemonic_defined_attributes (ident, value_type, cardinality)
VALUES ('txn/step', 'db.type/int8', 'db.cardinality/one');
INSERT INTO mnemonic_defined_attributes (ident, value_type, cardinality)
VALUES ('txn/type', 'db.type/text', 'db.cardinality/one');
INSERT INTO mnemonic_defined_attributes (ident, value_type, cardinality)
VALUES ('txn/amount', 'db.type/int8', 'db.cardinality/one');
INSERT INTO mnemonic_defined_attributes (ident, value_type, cardinality, doc)
VALUES ('txn/from-account', 'db.type/ref', 'db.cardinality/one', 'Source account for transfers/expenses');
INSERT INTO mnemonic_defined_attributes (ident, value_type, cardinality, doc)
VALUES ('txn/to-account', 'db.type/ref', 'db.cardinality/one', 'Destination account for transfers/income');
INSERT INTO mnemonic_defined_attributes (ident, value_type, cardinality)
VALUES ('txn/description', 'db.type/text', 'db.cardinality/one');
CALL mnemonic_commit_tx();`,

  // Block 2: Create views
  `CALL mnemonic_begin_tx();
INSERT INTO mnemonic_defined_views (name, attributes, doc)
VALUES ('accounts', ARRAY['account/name', 'account/balance', 'account/last-seen'], 'Bank accounts with balances');
INSERT INTO mnemonic_defined_views (name, attributes, optional_attributes, doc)
VALUES ('bank_transactions',
  ARRAY['txn/step', 'txn/type', 'txn/amount'],
  ARRAY['txn/from-account', 'txn/to-account', 'txn/description'],
  'Financial transaction records');
CALL mnemonic_commit_tx();`,

  // Block 3: Create step tracking table (outside MnemonicDB system)
  `CREATE TABLE IF NOT EXISTS simulation_steps (
  step INTEGER PRIMARY KEY,
  tx_id BIGINT NOT NULL
);`,
];

export const QUERIES = {
  getAccounts: `
    SELECT id, name, balance, last_seen
    FROM accounts
    ORDER BY name
  `,

  getTopAccounts: (limit: number = 5) => `
    SELECT id, name, balance, last_seen
    FROM accounts
    ORDER BY balance DESC
    LIMIT ${limit}
  `,

  getBottomAccounts: (limit: number = 5) => `
    SELECT id, name, balance, last_seen
    FROM accounts
    WHERE balance > 0
    ORDER BY balance ASC
    LIMIT ${limit}
  `,

  getTransactionsByStep: (step: number) => `
    SELECT
      bt.id,
      bt.step,
      bt.type,
      bt.amount,
      bt.from_account,
      bt.to_account,
      bt.description,
      fa.name as from_name,
      ta.name as to_name
    FROM bank_transactions bt
    LEFT JOIN accounts fa ON bt.from_account = fa.id
    LEFT JOIN accounts ta ON bt.to_account = ta.id
    WHERE bt.step = ${step}
    ORDER BY bt.id DESC
  `,

  getTransactions: (limit: number = 20) => `
    SELECT
      bt.id,
      bt.step,
      bt.type,
      bt.amount,
      bt.from_account,
      bt.to_account,
      bt.description,
      fa.name as from_name,
      ta.name as to_name
    FROM bank_transactions bt
    LEFT JOIN accounts fa ON bt.from_account = fa.id
    LEFT JOIN accounts ta ON bt.to_account = ta.id
    ORDER BY bt.step DESC, bt.id DESC
    LIMIT ${limit}
  `,

  getAggregates: `
    SELECT
      (SELECT COUNT(*)::integer FROM accounts) as total_accounts,
      (SELECT COALESCE(SUM(balance), 0)::bigint FROM accounts) as total_money,
      (SELECT COALESCE(AVG(balance), 0)::numeric FROM accounts) as avg_balance,
      (SELECT COUNT(*)::integer FROM bank_transactions) as total_transactions
  `,

  getLatestTxId: `
    SELECT id FROM transactions ORDER BY id DESC LIMIT 1
  `,

  recordStep: (step: number, txId: string) => `
    INSERT INTO simulation_steps (step, tx_id)
    VALUES (${step}, ${txId})
    ON CONFLICT (step) DO UPDATE SET tx_id = EXCLUDED.tx_id
  `,

  getStepTxId: (step: number) => `
    SELECT tx_id FROM simulation_steps WHERE step = ${step}
  `,

  setAsOfTx: (txId: string | null) =>
    txId
      ? `SELECT set_config('mnemonic.as_of_tx', '${txId}', false)`
      : `SELECT set_config('mnemonic.as_of_tx', '', false)`,
};
