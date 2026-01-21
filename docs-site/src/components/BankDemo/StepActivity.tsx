import { useMemo, type FC } from 'react';
import { UserPlus, ArrowDownCircle, ArrowUpCircle, ArrowRightLeft } from 'lucide-react';
import type { BankTransaction } from './types';

interface StepActivityProps {
  transactions: BankTransaction[];
  step: number;
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

const typeConfig: Record<string, { icon: React.ReactNode; color: string; label: string }> = {
  signup: { icon: <UserPlus size={16} />, color: '#05d9e8', label: 'New Account' },
  income: { icon: <ArrowDownCircle size={16} />, color: '#39ff14', label: 'Income' },
  expense: { icon: <ArrowUpCircle size={16} />, color: '#ff6b6b', label: 'Expense' },
  transfer: { icon: <ArrowRightLeft size={16} />, color: '#ffd700', label: 'Transfer' },
};

function getDescription(transaction: BankTransaction): string {
  switch (transaction.type) {
    case 'signup':
      return `${transaction.to_name} opened account with ${formatMoney(transaction.amount)}`;
    case 'income':
      return `${transaction.to_name} received ${formatMoney(transaction.amount)} - ${transaction.description}`;
    case 'expense':
      return `${transaction.from_name} spent ${formatMoney(transaction.amount)} - ${transaction.description}`;
    case 'transfer':
      return `${transaction.from_name} sent ${formatMoney(transaction.amount)} to ${transaction.to_name}`;
    default:
      return transaction.description || '';
  }
}

const MAX_VISIBLE_ITEMS = 10;
const ITEM_HEIGHT = 41;

interface ActivitySlotProps {
  transaction: BankTransaction | null;
  isLast: boolean;
}

const ActivitySlot: FC<ActivitySlotProps> = ({ transaction, isLast }) => {
  if (!transaction) {
    return (
      <div style={{
        height: `${ITEM_HEIGHT}px`,
        borderBottom: isLast ? 'none' : '1px solid rgba(160, 160, 192, 0.1)',
        margin: 0,
      }} />
    );
  }

  const config = typeConfig[transaction.type] || { icon: null, color: '#e0e0ff', label: transaction.type };

  return (
    <div style={{
      display: 'flex',
      alignItems: 'center',
      gap: '12px',
      padding: '10px 16px',
      height: `${ITEM_HEIGHT}px`,
      boxSizing: 'border-box',
      borderBottom: isLast ? 'none' : '1px solid rgba(160, 160, 192, 0.1)',
      margin: 0,
    }}>
      <span style={{ color: config.color, margin: 0 }}>
        {config.icon}
      </span>
      <span style={{
        fontSize: '0.7rem',
        fontWeight: 600,
        textTransform: 'uppercase',
        letterSpacing: '0.05em',
        color: config.color,
        minWidth: '70px',
        margin: 0,
      }}>
        {config.label}
      </span>
      <span style={{
        flex: 1,
        color: '#a0a0c0',
        fontSize: '0.9rem',
        overflow: 'hidden',
        textOverflow: 'ellipsis',
        whiteSpace: 'nowrap',
        margin: 0,
      }}>
        {getDescription(transaction)}
      </span>
      <span style={{
        fontFamily: 'monospace',
        fontWeight: 600,
        fontSize: '0.9rem',
        color: transaction.type === 'expense' ? '#ff6b6b' : '#39ff14',
        margin: 0,
      }}>
        {transaction.type === 'expense' ? '-' : '+'}{formatMoney(transaction.amount)}
      </span>
    </div>
  );
};

export const StepActivity: FC<StepActivityProps> = ({ transactions, step, isLoading }) => {
  // Create fixed slots - always render MAX_VISIBLE_ITEMS slots
  const slots = useMemo(() => {
    const result: (BankTransaction | null)[] = [];
    for (let i = 0; i < MAX_VISIBLE_ITEMS; i++) {
      result.push(transactions[i] || null);
    }
    return result;
  }, [transactions]);

  return (
    <div style={{
      background: 'rgba(22, 22, 42, 0.6)',
      borderRadius: '12px',
      border: '1px solid rgba(160, 160, 192, 0.2)',
      overflow: 'hidden',
      margin: 0,
    }}>
      <div style={{
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'space-between',
        padding: '12px 16px',
        borderBottom: '1px solid rgba(160, 160, 192, 0.2)',
        background: 'rgba(22, 22, 42, 0.4)',
        margin: 0,
      }}>
        <h3 style={{
          margin: 0,
          fontSize: '0.95rem',
          fontWeight: 600,
          color: '#e0e0ff',
        }}>
          Step {step} Activity
        </h3>
        <span style={{
          fontSize: '0.8rem',
          color: '#808090',
          background: 'rgba(160, 160, 192, 0.1)',
          padding: '4px 10px',
          borderRadius: '12px',
          margin: 0,
        }}>
          {transactions.length} {transactions.length === 1 ? 'event' : 'events'}
        </span>
      </div>

      <div style={{
        opacity: isLoading ? 0.6 : 1,
        transition: 'opacity 0.15s ease',
        margin: 0,
      }}>
        {step === 0 ? (
          <div style={{
            height: `${MAX_VISIBLE_ITEMS * ITEM_HEIGHT}px`,
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'center',
            color: '#808090',
            fontSize: '0.9rem',
          }}>
            Simulation starting...
          </div>
        ) : (
          slots.map((txn, index) => (
            <ActivitySlot
              key={index}
              transaction={txn}
              isLast={index === MAX_VISIBLE_ITEMS - 1}
            />
          ))
        )}
      </div>
    </div>
  );
};

export default StepActivity;
