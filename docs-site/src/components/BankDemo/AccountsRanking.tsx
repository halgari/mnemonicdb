import { useMemo, type FC } from 'react';
import { TrendingUp, TrendingDown } from 'lucide-react';
import type { Account } from './types';

interface AccountsRankingProps {
  topAccounts: Account[];
  bottomAccounts: Account[];
  currentStep: number;
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

const MAX_ITEMS = 5;
const ROW_HEIGHT = 37;

interface AccountSlotProps {
  account: Account | null;
  rank: number;
  currentStep: number;
  isTop: boolean;
  isLast: boolean;
}

const AccountSlot: FC<AccountSlotProps> = ({ account, rank, currentStep, isTop, isLast }) => {
  if (!account) {
    return (
      <div style={{
        height: `${ROW_HEIGHT}px`,
        borderBottom: isLast ? 'none' : '1px solid rgba(160, 160, 192, 0.1)',
        margin: 0,
      }} />
    );
  }

  const isRecent = account.last_seen !== null && currentStep - account.last_seen <= 1;

  return (
    <div style={{
      display: 'flex',
      alignItems: 'center',
      padding: '8px 12px',
      height: `${ROW_HEIGHT}px`,
      boxSizing: 'border-box',
      borderBottom: isLast ? 'none' : '1px solid rgba(160, 160, 192, 0.1)',
      background: isRecent ? 'rgba(255, 255, 255, 0.03)' : 'transparent',
      margin: 0,
    }}>
      <span style={{
        width: '24px',
        fontSize: '0.8rem',
        fontWeight: 700,
        color: isTop ? '#39ff14' : '#ff6b6b',
        fontFamily: 'monospace',
        margin: 0,
      }}>
        #{rank}
      </span>
      <span style={{
        flex: 1,
        color: '#e0e0ff',
        fontWeight: 500,
        fontSize: '0.9rem',
        overflow: 'hidden',
        textOverflow: 'ellipsis',
        whiteSpace: 'nowrap',
        margin: 0,
      }}>
        {account.name}
      </span>
      <span style={{
        color: isTop ? '#39ff14' : '#ff6b6b',
        fontFamily: 'monospace',
        fontWeight: 600,
        fontSize: '0.9rem',
        marginRight: '12px',
        marginTop: 0,
        marginBottom: 0,
        marginLeft: 0,
      }}>
        {formatMoney(account.balance)}
      </span>
      <span style={{
        fontSize: '0.7rem',
        color: isRecent ? '#05d9e8' : '#606080',
        minWidth: '50px',
        textAlign: 'right',
        margin: 0,
      }}>
        {account.last_seen !== null ? `Step ${account.last_seen}` : '-'}
      </span>
    </div>
  );
};

interface RankingListProps {
  title: string;
  icon: React.ReactNode;
  accounts: Account[];
  currentStep: number;
  isTop: boolean;
  isLoading: boolean;
}

const HEADER_HEIGHT = 45;
const BODY_HEIGHT = MAX_ITEMS * ROW_HEIGHT;

const RankingList: FC<RankingListProps> = ({ title, icon, accounts, currentStep, isTop, isLoading }) => {
  // Create fixed slots
  const slots = useMemo(() => {
    const result: (Account | null)[] = [];
    for (let i = 0; i < MAX_ITEMS; i++) {
      result.push(accounts[i] || null);
    }
    return result;
  }, [accounts]);

  return (
    <div style={{
      flex: '1 1 0',
      background: 'rgba(22, 22, 42, 0.6)',
      borderRadius: '12px',
      border: '1px solid rgba(160, 160, 192, 0.2)',
      overflow: 'hidden',
      margin: 0,
    }}>
      <div style={{
        display: 'flex',
        alignItems: 'center',
        gap: '8px',
        padding: '0 16px',
        height: `${HEADER_HEIGHT}px`,
        borderBottom: '1px solid rgba(160, 160, 192, 0.2)',
        background: 'rgba(22, 22, 42, 0.4)',
        margin: 0,
      }}>
        <span style={{ color: isTop ? '#39ff14' : '#ff6b6b', margin: 0 }}>{icon}</span>
        <h3 style={{
          margin: 0,
          fontSize: '0.95rem',
          fontWeight: 600,
          color: '#e0e0ff',
        }}>
          {title}
        </h3>
      </div>

      <div style={{
        height: `${BODY_HEIGHT}px`,
        opacity: isLoading ? 0.6 : 1,
        transition: 'opacity 0.15s ease',
        margin: 0,
      }}>
        {slots.map((account, index) => (
          <AccountSlot
            key={index}
            account={account}
            rank={index + 1}
            currentStep={currentStep}
            isTop={isTop}
            isLast={index === MAX_ITEMS - 1}
          />
        ))}
      </div>
    </div>
  );
};

export const AccountsRanking: FC<AccountsRankingProps> = ({
  topAccounts,
  bottomAccounts,
  currentStep,
  isLoading,
}) => {
  return (
    <div style={{
      display: 'flex',
      gap: '20px',
      margin: 0,
    }}>
      <RankingList
        title="Top 5 Balances"
        icon={<TrendingUp size={18} />}
        accounts={topAccounts}
        currentStep={currentStep}
        isTop={true}
        isLoading={isLoading}
      />
      <RankingList
        title="Bottom 5 Balances"
        icon={<TrendingDown size={18} />}
        accounts={bottomAccounts}
        currentStep={currentStep}
        isTop={false}
        isLoading={isLoading}
      />
    </div>
  );
};

export default AccountsRanking;
