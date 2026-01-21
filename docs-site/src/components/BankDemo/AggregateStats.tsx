import type { FC } from 'react';
import type { Aggregates } from './types';

interface AggregateStatsProps {
  aggregates: Aggregates | undefined;
  isLoading: boolean;
}

function formatMoney(amount: number | bigint): string {
  const num = typeof amount === 'bigint' ? Number(amount) : amount;
  return new Intl.NumberFormat('en-US', {
    style: 'currency',
    currency: 'USD',
    minimumFractionDigits: 0,
    maximumFractionDigits: 0,
  }).format(num);
}

interface StatCardProps {
  label: string;
  value: string | number;
  color: string;
  isLoading: boolean;
}

const CARD_HEIGHT = 70;
const LABEL_HEIGHT = 14;
const VALUE_HEIGHT = 30;

const StatCard: FC<StatCardProps> = ({ label, value, color, isLoading }) => (
  <div style={{
    flex: '1 1 0',
    display: 'flex',
    flexDirection: 'column',
    alignItems: 'center',
    justifyContent: 'center',
    height: `${CARD_HEIGHT}px`,
    padding: '0 16px',
    background: 'rgba(22, 22, 42, 0.4)',
    borderRadius: '8px',
    border: '1px solid rgba(160, 160, 192, 0.15)',
    boxSizing: 'border-box',
    margin: 0,
  }}>
    <span style={{
      height: `${LABEL_HEIGHT}px`,
      fontSize: '0.75rem',
      color: '#808090',
      textTransform: 'uppercase',
      letterSpacing: '0.05em',
      lineHeight: `${LABEL_HEIGHT}px`,
      margin: 0,
    }}>
      {label}
    </span>
    <span style={{
      height: `${VALUE_HEIGHT}px`,
      fontSize: '1.5rem',
      fontWeight: 700,
      color,
      opacity: isLoading ? 0.5 : 1,
      lineHeight: `${VALUE_HEIGHT}px`,
      marginTop: '4px',
      marginBottom: 0,
      marginLeft: 0,
      marginRight: 0,
    }}>
      {isLoading ? '...' : value}
    </span>
  </div>
);

export const AggregateStats: FC<AggregateStatsProps> = ({
  aggregates,
  isLoading,
}) => {
  const totalAccounts = aggregates?.total_accounts ?? 0;
  const totalMoney = aggregates?.total_money ?? 0;
  const avgBalance = aggregates?.avg_balance ?? 0;
  const totalTransactions = aggregates?.total_transactions ?? 0;

  return (
    <div style={{
      display: 'flex',
      gap: '12px',
      padding: '16px 20px',
      background: 'rgba(22, 22, 42, 0.6)',
      borderRadius: '12px',
      border: '1px solid rgba(160, 160, 192, 0.2)',
      alignItems: 'stretch',
      margin: 0,
    }}>
      <StatCard
        label="Accounts"
        value={totalAccounts}
        color="#05d9e8"
        isLoading={isLoading}
      />
      <StatCard
        label="Transactions"
        value={totalTransactions}
        color="#d300c5"
        isLoading={isLoading}
      />
      <StatCard
        label="Total Money"
        value={formatMoney(totalMoney)}
        color="#39ff14"
        isLoading={isLoading}
      />
      <StatCard
        label="Avg Balance"
        value={formatMoney(Number(avgBalance))}
        color="#ff9f1c"
        isLoading={isLoading}
      />
    </div>
  );
};

export default AggregateStats;
