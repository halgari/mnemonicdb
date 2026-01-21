import { useState, useEffect, useCallback, useRef, type FC } from 'react';
import { useQueryClient } from '@tanstack/react-query';
import { getDatabase } from '../../lib/pglite-loader';
import { SCHEMA_BLOCKS } from './bankSchema';
import { generateEvents, generateSignupSQL, generateEventSQL } from './SimulationEngine';
import {
  useTopAccounts,
  useBottomAccounts,
  useStepTransactions,
  useAggregates,
  useDatomCounts,
  recordStepTx,
  getStepTxId,
} from './useBankQueries';
import type { DemoState, Account, PerformanceStats as PerformanceStatsType } from './types';

import TimeControls from './TimeControls';
import AggregateStats from './AggregateStats';
import PerformanceStats from './PerformanceStats';
import AccountsRanking from './AccountsRanking';
import StepActivity from './StepActivity';

// Helper to compute rolling average
function updateAverage(current: number | null, newValue: number, count: number): number {
  if (current === null) return newValue;
  // Exponential moving average with more weight on recent values
  const alpha = Math.min(0.3, 2 / (count + 1));
  return current * (1 - alpha) + newValue * alpha;
}

export const BankDemoContent: FC = () => {
  const queryClient = useQueryClient();
  const [state, setState] = useState<DemoState>({
    simulationStep: 0,
    viewingStep: 0,
    isPlaying: false,
    isLive: true,
    isInitialized: false,
    error: null,
  });

  // Performance stats
  const [perfStats, setPerfStats] = useState<PerformanceStatsType>({
    lastInsertTimeMs: null,
    lastQueryTimeMs: null,
    avgInsertTimeMs: null,
    avgQueryTimeMs: null,
    totalDatoms: null,
    currentDatoms: null,
  });
  const insertCountRef = useRef(0);
  const queryCountRef = useRef(0);

  // Track tx IDs for each step
  const stepTxMap = useRef<Map<number, string>>(new Map());

  // Current viewing tx ID
  const [viewingTxId, setViewingTxId] = useState<string | null>(null);

  // Queries - pass null for live view, txId for historical
  const topAccountsQuery = useTopAccounts(
    state.isLive ? null : viewingTxId,
    5,
    state.isInitialized
  );
  const bottomAccountsQuery = useBottomAccounts(
    state.isLive ? null : viewingTxId,
    5,
    state.isInitialized
  );
  const stepTransactionsQuery = useStepTransactions(
    state.isLive ? null : viewingTxId,
    state.viewingStep,
    state.isInitialized
  );
  const aggregatesQuery = useAggregates(
    state.isLive ? null : viewingTxId,
    state.isInitialized
  );
  const datomCountsQuery = useDatomCounts(state.isInitialized);

  // Extract data
  const topAccounts = topAccountsQuery.data?.data ?? [];
  const bottomAccounts = bottomAccountsQuery.data?.data ?? [];
  const stepTransactions = stepTransactionsQuery.data?.data ?? [];
  const aggregates = aggregatesQuery.data?.data;

  // Update query timing when data changes
  useEffect(() => {
    const times: number[] = [];
    if (topAccountsQuery.data?.queryTimeMs) times.push(topAccountsQuery.data.queryTimeMs);
    if (bottomAccountsQuery.data?.queryTimeMs) times.push(bottomAccountsQuery.data.queryTimeMs);
    if (stepTransactionsQuery.data?.queryTimeMs) times.push(stepTransactionsQuery.data.queryTimeMs);
    if (aggregatesQuery.data?.queryTimeMs) times.push(aggregatesQuery.data.queryTimeMs);

    if (times.length > 0) {
      const totalQueryTime = times.reduce((a, b) => a + b, 0);
      queryCountRef.current++;
      setPerfStats(prev => ({
        ...prev,
        lastQueryTimeMs: totalQueryTime,
        avgQueryTimeMs: updateAverage(prev.avgQueryTimeMs, totalQueryTime, queryCountRef.current),
      }));
    }
  }, [topAccountsQuery.data, bottomAccountsQuery.data, stepTransactionsQuery.data, aggregatesQuery.data]);

  // Update datom counts when they change
  useEffect(() => {
    if (datomCountsQuery.data) {
      setPerfStats(prev => ({
        ...prev,
        totalDatoms: datomCountsQuery.data.total_datoms,
        currentDatoms: datomCountsQuery.data.current_datoms,
      }));
    }
  }, [datomCountsQuery.data]);

  // Initialize schema
  useEffect(() => {
    let cancelled = false;

    const initSchema = async () => {
      try {
        const db = await getDatabase();

        // Check if schema already exists by trying to query accounts
        try {
          await db.query('SELECT 1 FROM accounts LIMIT 1');
          // Schema exists, just set initialized
          if (!cancelled) {
            setState(s => ({ ...s, isInitialized: true, isPlaying: true }));
          }
          return;
        } catch {
          // Schema doesn't exist, create it
        }

        // Create the schema - execute each block as a unit to preserve transaction context
        for (const block of SCHEMA_BLOCKS) {
          await db.exec(block);
        }

        if (!cancelled) {
          setState(s => ({ ...s, isInitialized: true, isPlaying: true }));
        }
      } catch (err) {
        console.error('Schema init error:', err);
        if (!cancelled) {
          setState(s => ({
            ...s,
            error: `Failed to initialize: ${err instanceof Error ? err.message : String(err)}`,
          }));
        }
      }
    };

    initSchema();

    return () => {
      cancelled = true;
    };
  }, []);

  // Execute a simulation step
  const executeStep = useCallback(async () => {
    if (!state.isInitialized) return;

    try {
      const db = await getDatabase();
      const nextStep = state.simulationStep + 1;

      // Clear any as_of_tx to work with current data
      await db.exec(`SELECT set_config('mnemonic.as_of_tx', '', false)`);

      // Get current accounts for simulation
      const accountsResult = await db.query<Account>(
        'SELECT id, name, balance, last_seen FROM accounts'
      );
      const currentAccounts = accountsResult.rows;

      // Generate events for this step
      const events = generateEvents(currentAccounts, nextStep);

      // Build all SQL statements and execute as a single batch to preserve transaction context
      const allStatements: string[] = ['CALL mnemonic_begin_tx();'];

      for (const event of events) {
        if (event.type === 'signup') {
          allStatements.push(generateSignupSQL(event.name, event.initialBalance, nextStep) + ';');
        } else {
          const statements = generateEventSQL(event, nextStep);
          for (const stmt of statements) {
            allStatements.push(stmt + ';');
          }
        }
      }

      allStatements.push('CALL mnemonic_commit_tx();');

      // Execute all statements as a single batch and measure time
      const insertStart = performance.now();
      await db.exec(allStatements.join('\n'));
      const insertTimeMs = performance.now() - insertStart;

      // Update insert timing stats
      insertCountRef.current++;
      setPerfStats(prev => ({
        ...prev,
        lastInsertTimeMs: insertTimeMs,
        avgInsertTimeMs: updateAverage(prev.avgInsertTimeMs, insertTimeMs, insertCountRef.current),
      }));

      // Record this step's transaction ID
      const txId = await recordStepTx(nextStep);
      stepTxMap.current.set(nextStep, txId);

      // Update state
      setState(s => ({
        ...s,
        simulationStep: nextStep,
        viewingStep: s.isLive ? nextStep : s.viewingStep,
      }));

      // Invalidate queries to refresh data
      await queryClient.invalidateQueries({ queryKey: ['topAccounts'] });
      await queryClient.invalidateQueries({ queryKey: ['bottomAccounts'] });
      await queryClient.invalidateQueries({ queryKey: ['stepTransactions'] });
      await queryClient.invalidateQueries({ queryKey: ['aggregates'] });
      await queryClient.invalidateQueries({ queryKey: ['datomCounts'] });

    } catch (err) {
      console.error('Step execution error:', err);
      setState(s => ({
        ...s,
        error: `Step failed: ${err instanceof Error ? err.message : String(err)}`,
        isPlaying: false,
      }));
    }
  }, [state.isInitialized, state.simulationStep, state.isLive, queryClient]);

  // Auto-play timer (250ms per step for faster simulation)
  useEffect(() => {
    if (!state.isPlaying || !state.isInitialized) return;

    const timer = setInterval(executeStep, 250);
    return () => clearInterval(timer);
  }, [state.isPlaying, state.isInitialized, executeStep]);

  // Handle seeking to a step
  const handleSeek = useCallback(async (step: number) => {
    // Get tx ID for this step
    let txId: string | null = stepTxMap.current.get(step) ?? null;
    if (!txId) {
      txId = await getStepTxId(step);
      if (txId) {
        stepTxMap.current.set(step, txId);
      }
    }

    setViewingTxId(txId);
    setState(s => ({
      ...s,
      viewingStep: step,
      isLive: step === s.simulationStep,
    }));
  }, []);

  // Resume live view
  const handleResumeLive = useCallback(() => {
    setViewingTxId(null);
    setState(s => ({
      ...s,
      viewingStep: s.simulationStep,
      isLive: true,
    }));
  }, []);

  // Step forward manually
  const handleStepForward = useCallback(async () => {
    if (state.viewingStep < state.simulationStep) {
      // Just move viewing forward
      handleSeek(state.viewingStep + 1);
    } else {
      // Execute a new step
      await executeStep();
    }
  }, [state.viewingStep, state.simulationStep, handleSeek, executeStep]);

  // Step backward
  const handleStepBack = useCallback(() => {
    if (state.viewingStep > 0) {
      handleSeek(state.viewingStep - 1);
    }
  }, [state.viewingStep, handleSeek]);

  if (state.error) {
    return (
      <div style={{
        padding: '20px',
        background: 'rgba(255, 68, 68, 0.1)',
        borderRadius: '12px',
        border: '1px solid rgba(255, 68, 68, 0.3)',
        color: '#ff6b6b',
      }}>
        <strong>Error:</strong> {state.error}
      </div>
    );
  }

  if (!state.isInitialized) {
    return (
      <div style={{
        padding: '40px',
        textAlign: 'center',
        color: '#a0a0c0',
      }}>
        Initializing bank simulation...
      </div>
    );
  }

  const isLoading = topAccountsQuery.isLoading || bottomAccountsQuery.isLoading ||
                    stepTransactionsQuery.isLoading || aggregatesQuery.isLoading;

  return (
    <div style={{
      display: 'flex',
      flexDirection: 'column',
      gap: '20px',
      padding: '4px',
      borderRadius: '16px',
      border: state.isLive
        ? '2px solid transparent'
        : '2px solid rgba(255, 215, 0, 0.4)',
      transition: 'border-color 0.3s ease',
    }}>
      {!state.isLive && (
        <div style={{
          padding: '8px 16px',
          background: 'rgba(255, 215, 0, 0.1)',
          borderRadius: '8px',
          border: '1px solid rgba(255, 215, 0, 0.3)',
          color: '#ffd700',
          fontSize: '0.85rem',
          textAlign: 'center',
        }}>
          Viewing historical state at Step {state.viewingStep}. The simulation continues in the background.
        </div>
      )}

      <TimeControls
        simulationStep={state.simulationStep}
        viewingStep={state.viewingStep}
        isPlaying={state.isPlaying}
        isLive={state.isLive}
        onPlay={() => setState(s => ({ ...s, isPlaying: true }))}
        onPause={() => setState(s => ({ ...s, isPlaying: false }))}
        onStepForward={handleStepForward}
        onStepBack={handleStepBack}
        onSeek={handleSeek}
        onResumeLive={handleResumeLive}
      />

      <AggregateStats aggregates={aggregates} isLoading={isLoading} />

      <PerformanceStats stats={perfStats} />

      <StepActivity
        transactions={stepTransactions}
        step={state.viewingStep}
        isLoading={isLoading}
      />

      <AccountsRanking
        topAccounts={topAccounts}
        bottomAccounts={bottomAccounts}
        currentStep={state.viewingStep}
        isLoading={isLoading}
      />
    </div>
  );
};

export default BankDemoContent;
