// Bank Demo Types

export interface Account {
  id: number;
  name: string;
  balance: number;
  last_seen: number | null;
}

export interface BankTransaction {
  id: number;
  step: number;
  type: 'signup' | 'income' | 'expense' | 'transfer';
  amount: number;
  from_account?: number;
  to_account?: number;
  from_name?: string;
  to_name?: string;
  description?: string;
}

export interface Aggregates {
  total_accounts: number;
  total_money: number;
  avg_balance: number;
  total_transactions: number;
}

export interface StepInfo {
  step: number;
  tx_id: string;
}

export type SimEvent =
  | { type: 'signup'; name: string; initialBalance: number }
  | { type: 'income'; accountId: number; amount: number; description: string }
  | { type: 'expense'; accountId: number; amount: number; description: string }
  | { type: 'transfer'; fromId: number; toId: number; amount: number; description: string };

export interface DemoState {
  simulationStep: number;
  viewingStep: number;
  isPlaying: boolean;
  isLive: boolean;
  isInitialized: boolean;
  error: string | null;
}

export interface PerformanceStats {
  lastInsertTimeMs: number | null;
  lastQueryTimeMs: number | null;
  avgInsertTimeMs: number | null;
  avgQueryTimeMs: number | null;
  totalDatoms: number | null;
  currentDatoms: number | null;
}
