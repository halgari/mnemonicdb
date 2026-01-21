// TanStack Query hooks for Bank Demo
import { useQuery } from '@tanstack/react-query';
import { getDatabase, executeQuery } from '../../lib/pglite-loader';
import { QUERIES } from './bankSchema';
import type { Account, BankTransaction, Aggregates } from './types';

// Helper to set the as_of_tx for time-travel queries
async function setAsOfTx(txId: string | null): Promise<void> {
  const db = await getDatabase();
  await db.exec(QUERIES.setAsOfTx(txId));
}

// Result type with timing info
export interface TimedQueryResult<T> {
  data: T;
  queryTimeMs: number;
}

// Hook to fetch all accounts with timing
export function useAccounts(asOfTxId: string | null, enabled: boolean = true) {
  return useQuery({
    queryKey: ['accounts', asOfTxId],
    queryFn: async (): Promise<TimedQueryResult<Account[]>> => {
      await setAsOfTx(asOfTxId);
      const start = performance.now();
      const result = await executeQuery<Account>(QUERIES.getAccounts);
      const queryTimeMs = performance.now() - start;
      return { data: result.rows, queryTimeMs };
    },
    enabled,
    staleTime: 0,
  });
}

// Hook to fetch top N accounts by balance
export function useTopAccounts(asOfTxId: string | null, limit: number = 5, enabled: boolean = true) {
  return useQuery({
    queryKey: ['topAccounts', asOfTxId, limit],
    queryFn: async (): Promise<TimedQueryResult<Account[]>> => {
      await setAsOfTx(asOfTxId);
      const start = performance.now();
      const result = await executeQuery<Account>(QUERIES.getTopAccounts(limit));
      const queryTimeMs = performance.now() - start;
      return { data: result.rows, queryTimeMs };
    },
    enabled,
    staleTime: 0,
  });
}

// Hook to fetch bottom N accounts by balance
export function useBottomAccounts(asOfTxId: string | null, limit: number = 5, enabled: boolean = true) {
  return useQuery({
    queryKey: ['bottomAccounts', asOfTxId, limit],
    queryFn: async (): Promise<TimedQueryResult<Account[]>> => {
      await setAsOfTx(asOfTxId);
      const start = performance.now();
      const result = await executeQuery<Account>(QUERIES.getBottomAccounts(limit));
      const queryTimeMs = performance.now() - start;
      return { data: result.rows, queryTimeMs };
    },
    enabled,
    staleTime: 0,
  });
}

// Hook to fetch transactions for a specific step
export function useStepTransactions(asOfTxId: string | null, step: number, enabled: boolean = true) {
  return useQuery({
    queryKey: ['stepTransactions', asOfTxId, step],
    queryFn: async (): Promise<TimedQueryResult<BankTransaction[]>> => {
      await setAsOfTx(asOfTxId);
      const start = performance.now();
      const result = await executeQuery<BankTransaction>(QUERIES.getTransactionsByStep(step));
      const queryTimeMs = performance.now() - start;
      return { data: result.rows, queryTimeMs };
    },
    enabled: enabled && step > 0,
    staleTime: 0,
  });
}

// Hook to fetch transactions (legacy, for history view)
export function useTransactions(asOfTxId: string | null, limit: number = 20, enabled: boolean = true) {
  return useQuery({
    queryKey: ['transactions', asOfTxId, limit],
    queryFn: async (): Promise<TimedQueryResult<BankTransaction[]>> => {
      await setAsOfTx(asOfTxId);
      const start = performance.now();
      const result = await executeQuery<BankTransaction>(QUERIES.getTransactions(limit));
      const queryTimeMs = performance.now() - start;
      return { data: result.rows, queryTimeMs };
    },
    enabled,
    staleTime: 0,
  });
}

// Hook to fetch aggregates with timing
export function useAggregates(asOfTxId: string | null, enabled: boolean = true) {
  return useQuery({
    queryKey: ['aggregates', asOfTxId],
    queryFn: async (): Promise<TimedQueryResult<Aggregates>> => {
      await setAsOfTx(asOfTxId);
      const start = performance.now();
      const result = await executeQuery<Aggregates>(QUERIES.getAggregates);
      const queryTimeMs = performance.now() - start;
      const data = result.rows[0] || { total_accounts: 0, total_money: 0, avg_balance: 0, total_transactions: 0 };
      return { data, queryTimeMs };
    },
    enabled,
    staleTime: 0,
  });
}

// Get the transaction ID for a given step
export async function getStepTxId(step: number): Promise<string | null> {
  try {
    const result = await executeQuery<{ tx_id: string }>(QUERIES.getStepTxId(step));
    return result.rows[0]?.tx_id?.toString() || null;
  } catch {
    return null;
  }
}

// Record a step's transaction ID
export async function recordStepTx(step: number): Promise<string> {
  const db = await getDatabase();
  const result = await db.query<{ id: bigint }>(QUERIES.getLatestTxId);
  const txId = result.rows[0]?.id?.toString() || '0';
  await db.exec(QUERIES.recordStep(step, txId));
  return txId;
}
