import type { FC } from 'react';
import type { PerformanceStats as PerformanceStatsType } from './types';
import { Database, Search } from 'lucide-react';

interface PerformanceStatsProps {
  stats: PerformanceStatsType;
}

function formatTime(ms: number | null): string {
  if (ms === null) return '-';
  if (ms < 1) return '<1ms';
  return `${ms.toFixed(1)}ms`;
}

interface StatItemProps {
  icon: React.ReactNode;
  label: string;
  lastValue: number | null;
  avgValue: number | null;
  color: string;
}

const StatItem: FC<StatItemProps> = ({ icon, label, lastValue, avgValue, color }) => (
  <div style={{
    display: 'flex',
    flexDirection: 'row',
    alignItems: 'center',
    gap: '12px',
    padding: '12px 16px',
    boxSizing: 'border-box',
    background: 'rgba(22, 22, 42, 0.4)',
    borderRadius: '8px',
    border: '1px solid rgba(160, 160, 192, 0.15)',
    margin: 0,
  }}>
    <div style={{
      width: '18px',
      height: '18px',
      display: 'flex',
      alignItems: 'center',
      justifyContent: 'center',
      color,
      opacity: 0.8,
      flexShrink: 0,
      margin: 0,
    }}>
      {icon}
    </div>
    <div style={{
      display: 'flex',
      flexDirection: 'column',
      justifyContent: 'center',
      height: '100%',
      margin: 0,
    }}>
      <span style={{
        fontSize: '0.7rem',
        color: '#808090',
        textTransform: 'uppercase',
        letterSpacing: '0.05em',
        lineHeight: '1',
        display: 'block',
        margin: 0,
      }}>
        {label}
      </span>
      <span style={{
        fontSize: '1.1rem',
        fontWeight: 700,
        fontFamily: 'monospace',
        color,
        lineHeight: '1',
        marginTop: '4px',
        marginBottom: 0,
        marginLeft: 0,
        marginRight: 0,
        display: 'block',
      }}>
        {formatTime(lastValue)}
        <span style={{
          fontSize: '0.75rem',
          fontWeight: 400,
          fontFamily: 'system-ui, sans-serif',
          color: '#808090',
          marginLeft: '12px',
          marginTop: 0,
          marginBottom: 0,
          marginRight: 0,
        }}>
          avg: {formatTime(avgValue)}
        </span>
      </span>
    </div>
  </div>
);

export const PerformanceStats: FC<PerformanceStatsProps> = ({ stats }) => {
  return (
    <div style={{
      display: 'flex',
      flexDirection: 'row',
      gap: '12px',
      padding: '12px 16px',
      background: 'rgba(22, 22, 42, 0.6)',
      borderRadius: '12px',
      border: '1px solid rgba(160, 160, 192, 0.2)',
      margin: 0,
    }}>
      <div style={{
        fontSize: '0.7rem',
        color: '#606080',
        textTransform: 'uppercase',
        letterSpacing: '0.05em',
        writingMode: 'vertical-rl',
        textOrientation: 'mixed',
        transform: 'rotate(180deg)',
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
        flexShrink: 0,
      }}>
        Performance
      </div>
      <div style={{
        display: 'grid',
        gridTemplateColumns: '1fr 1fr',
        gap: '12px',
        flex: 1,
        margin: 0,
        alignItems: 'stretch',
      }}>
        <StatItem
          icon={<Database size={18} />}
          label="Insert Time"
          lastValue={stats.lastInsertTimeMs}
          avgValue={stats.avgInsertTimeMs}
          color="#ff2a6d"
        />
        <StatItem
          icon={<Search size={18} />}
          label="Query Time"
          lastValue={stats.lastQueryTimeMs}
          avgValue={stats.avgQueryTimeMs}
          color="#05d9e8"
        />
      </div>
    </div>
  );
};

export default PerformanceStats;
