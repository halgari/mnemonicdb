# MnemonicDB Bank Simulation Demo - Implementation Plan

## Overview

A live, interactive bank simulation demonstrating MnemonicDB's time-travel capabilities, relational aggregates, and reactive query patterns. Users can watch a simulation unfold in real-time and scrub backwards through history to see the database state at any point in time.

---

## Technology Stack

### Migration: Preact → React

The docs site currently uses Preact. We'll migrate to React to enable full TanStack ecosystem compatibility.

**Changes Required:**

1. **Install React packages:**
```bash
npm install @astrojs/react react react-dom @types/react @types/react-dom
npm uninstall @astrojs/preact preact
```

2. **Update `astro.config.mjs`:**
```javascript
import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';
import react from '@astrojs/react';  // Changed from preact
import tailwind from '@astrojs/tailwind';

export default defineConfig({
  // ... rest of config
  integrations: [
    starlight({ /* ... */ }),
    react(),  // Changed from preact()
    tailwind({ applyBaseStyles: false }),
  ],
});
```

3. **Update `tsconfig.json`:**
```json
{
  "extends": "astro/tsconfigs/strict",
  "compilerOptions": {
    "jsx": "react-jsx",
    "jsxImportSource": "react"
  }
}
```

4. **Update component imports:**
   - Change `/** @jsxImportSource preact */` → remove (React is default)
   - Change `from 'preact/hooks'` → `from 'react'`
   - Change `class=` → `className=` in JSX
   - Change `FunctionalComponent` → `FC` from React

---

## Component Libraries

### 1. TanStack Table (Data Grids)
**Package:** `@tanstack/react-table`

Headless table library - we provide the markup/styling, it provides the logic.

```bash
npm install @tanstack/react-table
```

**Basic Pattern:**
```tsx
import {
  useReactTable,
  getCoreRowModel,
  flexRender,
  createColumnHelper,
} from '@tanstack/react-table';

const columnHelper = createColumnHelper<Account>();

const columns = [
  columnHelper.accessor('name', { header: 'Name' }),
  columnHelper.accessor('balance', {
    header: 'Balance',
    cell: info => `$${info.getValue().toLocaleString()}`,
  }),
];

function AccountsTable({ data }: { data: Account[] }) {
  const table = useReactTable({
    data,
    columns,
    getCoreRowModel: getCoreRowModel(),
  });

  return (
    <table>
      <thead>
        {table.getHeaderGroups().map(headerGroup => (
          <tr key={headerGroup.id}>
            {headerGroup.headers.map(header => (
              <th key={header.id}>
                {flexRender(header.column.columnDef.header, header.getContext())}
              </th>
            ))}
          </tr>
        ))}
      </thead>
      <tbody>
        {table.getRowModel().rows.map(row => (
          <tr key={row.id}>
            {row.getVisibleCells().map(cell => (
              <td key={cell.id}>
                {flexRender(cell.column.columnDef.cell, cell.getContext())}
              </td>
            ))}
          </tr>
        ))}
      </tbody>
    </table>
  );
}
```

### 2. TanStack Query (Data Fetching & Reactivity)
**Package:** `@tanstack/react-query`

Provides caching, automatic refetching, and reactive data patterns.

```bash
npm install @tanstack/react-query
```

**Setup:**
```tsx
// In a provider wrapper component
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';

const queryClient = new QueryClient({
  defaultOptions: {
    queries: {
      refetchInterval: false, // We'll control refetching manually
      staleTime: 0,
    },
  },
});

function App({ children }) {
  return (
    <QueryClientProvider client={queryClient}>
      {children}
    </QueryClientProvider>
  );
}
```

**Usage for Live Queries:**
```tsx
import { useQuery, useQueryClient } from '@tanstack/react-query';

function useAccounts(asOfTx?: number) {
  return useQuery({
    queryKey: ['accounts', asOfTx],
    queryFn: async () => {
      const db = await getDatabase();
      if (asOfTx) {
        await db.exec(`SELECT set_config('mnemonic.as_of_tx', '${asOfTx}', false)`);
      } else {
        await db.exec(`SELECT set_config('mnemonic.as_of_tx', '', false)`);
      }
      const result = await db.query('SELECT id, name, balance FROM accounts ORDER BY name');
      return result.rows;
    },
  });
}

// Trigger refetch when simulation advances
const queryClient = useQueryClient();
queryClient.invalidateQueries({ queryKey: ['accounts'] });
```

### 3. rc-slider (Time Scrubber)
**Package:** `rc-slider`

Lightweight, customizable slider component.

```bash
npm install rc-slider
```

**Usage:**
```tsx
import Slider from 'rc-slider';
import 'rc-slider/assets/index.css';

function TimeSlider({
  currentStep,
  maxStep,
  onChange,
}: {
  currentStep: number;
  maxStep: number;
  onChange: (step: number) => void;
}) {
  return (
    <Slider
      min={0}
      max={maxStep}
      value={currentStep}
      onChange={(value) => onChange(value as number)}
      trackStyle={{ backgroundColor: '#ff2a6d' }}
      handleStyle={{ borderColor: '#ff2a6d', backgroundColor: '#d300c5' }}
      railStyle={{ backgroundColor: '#3a3a5c' }}
    />
  );
}
```

### 4. Lucide React (Icons)
**Package:** `lucide-react`

Clean, customizable icons for play/pause/forward/back buttons.

```bash
npm install lucide-react
```

**Usage:**
```tsx
import { Play, Pause, SkipBack, SkipForward, RotateCcw } from 'lucide-react';

<button onClick={togglePlay}>
  {isPlaying ? <Pause size={20} /> : <Play size={20} />}
</button>
```

---

## Complete Package.json Dependencies

```json
{
  "dependencies": {
    "@astrojs/react": "^4.0.0",
    "@astrojs/starlight": "^0.32.0",
    "@astrojs/starlight-tailwind": "^3.0.0",
    "@astrojs/tailwind": "^6.0.0",
    "@electric-sql/pglite": "^0.3.15",
    "@tanstack/react-query": "^5.60.0",
    "@tanstack/react-table": "^8.20.0",
    "astro": "^5.0.0",
    "lucide-react": "^0.460.0",
    "rc-slider": "^11.1.0",
    "react": "^18.3.0",
    "react-dom": "^18.3.0",
    "sharp": "^0.33.0",
    "tailwindcss": "^3.4.0"
  },
  "devDependencies": {
    "@types/react": "^18.3.0",
    "@types/react-dom": "^18.3.0"
  }
}
```

---

## Database Schema

### Entity Attributes

```sql
-- Account entity attributes
INSERT INTO mnemonic_defined_attributes (ident, value_type, cardinality)
VALUES
  ('account/name', 'db.type/text', 'db.cardinality/one'),
  ('account/balance', 'db.type/bigint', 'db.cardinality/one');

-- Transaction entity attributes (financial transactions)
INSERT INTO mnemonic_defined_attributes (ident, value_type, cardinality)
VALUES
  ('bank.txn/from-account', 'db.type/ref', 'db.cardinality/one'),
  ('bank.txn/to-account', 'db.type/ref', 'db.cardinality/one'),
  ('bank.txn/amount', 'db.type/bigint', 'db.cardinality/one'),
  ('bank.txn/type', 'db.type/text', 'db.cardinality/one'),
  ('bank.txn/description', 'db.type/text', 'db.cardinality/one'),
  ('bank.txn/step', 'db.type/bigint', 'db.cardinality/one');
```

### Views

```sql
-- Accounts view
INSERT INTO mnemonic_defined_views (name, attributes, doc)
VALUES ('accounts', ARRAY['account/name', 'account/balance'], 'Bank accounts');

-- Bank transactions view
INSERT INTO mnemonic_defined_views (name, attributes, optional_attributes, doc)
VALUES ('bank_transactions',
  ARRAY['bank.txn/amount', 'bank.txn/type', 'bank.txn/step'],
  ARRAY['bank.txn/from-account', 'bank.txn/to-account', 'bank.txn/description'],
  'Financial transactions');
```

### Step Tracking (Outside MnemonicDB)

```sql
-- Simple table to map simulation steps to MnemonicDB transaction IDs
CREATE TABLE IF NOT EXISTS simulation_steps (
  step INTEGER PRIMARY KEY,
  tx_id BIGINT NOT NULL,
  created_at TIMESTAMPTZ DEFAULT NOW()
);
```

---

## Component Architecture

```
src/
├── components/
│   ├── BankDemo/
│   │   ├── BankDemo.tsx           # Main container with QueryClientProvider
│   │   ├── BankDemoContent.tsx    # Inner content (uses hooks)
│   │   ├── TimeControls.tsx       # Slider + play/pause/step controls
│   │   ├── AccountsGrid.tsx       # TanStack Table for accounts
│   │   ├── TransactionsGrid.tsx   # TanStack Table for transactions
│   │   ├── AggregateStats.tsx     # Live totals display
│   │   ├── SimulationEngine.ts    # Background simulation logic
│   │   ├── useBankQueries.ts      # TanStack Query hooks
│   │   ├── bankSchema.ts          # Schema initialization SQL
│   │   └── types.ts               # TypeScript interfaces
│   ├── CodeRunner.tsx             # Updated to React
│   └── ResultsTable.tsx           # Updated to React
├── lib/
│   └── pglite-loader.ts           # No changes needed
└── content/docs/examples/
    └── bank-demo.mdx              # Documentation page
```

---

## Component Specifications

### 1. BankDemo.tsx (Provider Wrapper)

```tsx
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { BankDemoContent } from './BankDemoContent';

const queryClient = new QueryClient();

export function BankDemo() {
  return (
    <QueryClientProvider client={queryClient}>
      <BankDemoContent />
    </QueryClientProvider>
  );
}
```

### 2. BankDemoContent.tsx (Main Logic)

**State:**
```typescript
interface DemoState {
  simulationStep: number;     // Current simulation step (always advancing)
  viewingStep: number;        // Step being displayed (can be historical)
  isPlaying: boolean;         // Is simulation running
  isLive: boolean;            // Is viewing current step (not historical)
  stepToTxMap: Map<number, bigint>; // Maps step → MnemonicDB tx ID
}
```

**Key Logic:**
- Simulation timer (1 second interval when playing)
- Track step-to-transaction mapping after each step
- Time-travel by setting `mnemonic.as_of_tx` config
- Invalidate TanStack Query cache on step change

### 3. TimeControls.tsx

```tsx
interface TimeControlsProps {
  simulationStep: number;
  viewingStep: number;
  isPlaying: boolean;
  isLive: boolean;
  onPlay: () => void;
  onPause: () => void;
  onStepForward: () => void;
  onStepBack: () => void;
  onSeek: (step: number) => void;
  onResumeLive: () => void;
}
```

**Layout:**
```
┌─────────────────────────────────────────────────────────────────────────┐
│  [⏮] [⏵/⏸] [⏭]   ════════════●══════════════   [● LIVE]              │
│                     Step 42 of 150                                      │
└─────────────────────────────────────────────────────────────────────────┘
```

- Step back/forward buttons
- Play/pause toggle
- rc-slider for scrubbing
- "LIVE" button (red dot, pulses when live) to resume current state

### 4. AccountsGrid.tsx

Uses TanStack Table with columns: ID, Name, Balance

```tsx
const columns = [
  columnHelper.accessor('id', { header: 'ID' }),
  columnHelper.accessor('name', { header: 'Name' }),
  columnHelper.accessor('balance', {
    header: 'Balance',
    cell: info => `$${info.getValue().toLocaleString()}`,
  }),
];
```

### 5. TransactionsGrid.tsx

Uses TanStack Table with columns: Step, Type, From, To, Amount, Description

Shows most recent 20 transactions for the viewed step.

### 6. AggregateStats.tsx

Displays live aggregates:
- Total Accounts
- Total Money in System
- Average Balance

```tsx
function useAggregates(asOfTx?: bigint) {
  return useQuery({
    queryKey: ['aggregates', asOfTx?.toString()],
    queryFn: async () => {
      // Set as_of_tx if viewing historical
      const result = await executeQuery(`
        SELECT
          COUNT(*) as total_accounts,
          COALESCE(SUM(balance), 0) as total_money,
          COALESCE(AVG(balance), 0) as avg_balance
        FROM accounts
      `);
      return result.rows[0];
    },
  });
}
```

### 7. SimulationEngine.ts

**Event Generation:**
```typescript
type SimEvent =
  | { type: 'signup'; name: string; initialBalance: number }
  | { type: 'income'; accountId: number; amount: number }
  | { type: 'expense'; accountId: number; amount: number }
  | { type: 'transfer'; fromId: number; toId: number; amount: number };

function generateEvents(accounts: Account[], step: number): SimEvent[] {
  const events: SimEvent[] = [];
  const rng = seededRandom(step); // Deterministic for reproducibility

  // 15% chance of new signup (or 100% if no accounts)
  if (accounts.length === 0 || rng() < 0.15) {
    events.push({
      type: 'signup',
      name: generateName(rng),
      initialBalance: Math.floor(rng() * 900) + 100, // $100-$1000
    });
  }

  // Each account may do something
  for (const account of accounts) {
    const roll = rng();
    if (roll < 0.4) {
      // 40% - nothing
      continue;
    } else if (roll < 0.6) {
      // 20% - income ($50-$500)
      events.push({
        type: 'income',
        accountId: account.id,
        amount: Math.floor(rng() * 450) + 50,
      });
    } else if (roll < 0.8 && account.balance > 50) {
      // 20% - expense (up to 30% of balance, max $200)
      const maxExpense = Math.min(account.balance * 0.3, 200);
      events.push({
        type: 'expense',
        accountId: account.id,
        amount: Math.floor(rng() * maxExpense) + 10,
      });
    } else if (accounts.length > 1 && account.balance > 100) {
      // 20% - transfer to random other account
      const others = accounts.filter(a => a.id !== account.id);
      const target = others[Math.floor(rng() * others.length)];
      events.push({
        type: 'transfer',
        fromId: account.id,
        toId: target.id,
        amount: Math.floor(rng() * Math.min(account.balance * 0.2, 100)) + 10,
      });
    }
  }

  return events;
}
```

**Name Generator:**
```typescript
const FIRST_NAMES = ['Alice', 'Bob', 'Carol', 'David', 'Emma', 'Frank', 'Grace', 'Henry', 'Ivy', 'Jack', 'Kate', 'Leo', 'Maya', 'Noah', 'Olivia', 'Paul'];
const LAST_NAMES = ['Chen', 'Smith', 'Johnson', 'Williams', 'Brown', 'Jones', 'Garcia', 'Miller', 'Davis', 'Rodriguez', 'Martinez', 'Wilson', 'Anderson', 'Taylor'];

function generateName(rng: () => number): string {
  const first = FIRST_NAMES[Math.floor(rng() * FIRST_NAMES.length)];
  const last = LAST_NAMES[Math.floor(rng() * LAST_NAMES.length)];
  return `${first} ${last}`;
}
```

---

## Time-Travel Implementation

### Setting Historical View

```typescript
async function setAsOfTransaction(txId: bigint | null) {
  const db = await getDatabase();
  if (txId) {
    await db.exec(`SELECT set_config('mnemonic.as_of_tx', '${txId}', false)`);
  } else {
    // Clear to view current state
    await db.exec(`SELECT set_config('mnemonic.as_of_tx', '', false)`);
  }
}
```

### Recording Step → Transaction Mapping

After each simulation step commits:

```typescript
async function recordStepTransaction(step: number): Promise<bigint> {
  const db = await getDatabase();

  // Get the transaction ID that was just committed
  const result = await db.query(`
    SELECT id FROM transactions ORDER BY id DESC LIMIT 1
  `);
  const txId = result.rows[0].id as bigint;

  // Record mapping
  await db.exec(`
    INSERT INTO simulation_steps (step, tx_id)
    VALUES (${step}, ${txId})
    ON CONFLICT (step) DO UPDATE SET tx_id = EXCLUDED.tx_id
  `);

  return txId;
}
```

### Navigating History

```typescript
async function viewStep(step: number, stepToTxMap: Map<number, bigint>) {
  const txId = stepToTxMap.get(step);
  if (txId) {
    await setAsOfTransaction(txId);
  }
  // Then invalidate queries to refetch with new as_of_tx
  queryClient.invalidateQueries();
}

async function resumeLive() {
  await setAsOfTransaction(null);
  queryClient.invalidateQueries();
}
```

---

## Visual Design

### Color Scheme (Synthwave Theme)

| Element | Color | CSS |
|---------|-------|-----|
| Primary Action | Pink gradient | `linear-gradient(135deg, #ff2a6d, #d300c5)` |
| Secondary/Accent | Cyan | `#05d9e8` |
| Background | Dark purple | `#1a1a2e` |
| Card Background | Darker purple | `#16162a` |
| Text Primary | Light lavender | `#e0e0ff` |
| Text Muted | Gray lavender | `#a0a0c0` |
| Live Indicator | Red | `#ff4444` |
| Historical Mode | Amber | `#ffd700` |

### Layout (Desktop)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  🏦 MnemonicDB Bank Simulation                                              │
│  Watch time-travel and temporal queries in action                           │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─ Time Controls ────────────────────────────────────────────────────────┐ │
│  │  [⏮] [▶] [⏭]    ═══════════════●═══════════════    [● LIVE]           │ │
│  │                        Step 42 / 150                                   │ │
│  └────────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
│  ┌─ System Overview ──────────────────────────────────────────────────────┐ │
│  │  💰 Total Accounts: 12    💵 Total Money: $47,291    📊 Avg: $3,941    │ │
│  └────────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
│  ┌─ Accounts ──────────────────────┐  ┌─ Recent Activity ────────────────┐  │
│  │ ID │ Name           │ Balance   │  │ Step │ Type     │ Amount │ Desc  │  │
│  ├────┼────────────────┼───────────┤  ├──────┼──────────┼────────┼───────┤  │
│  │  1 │ Alice Chen     │   $5,234  │  │   42 │ transfer │   $150 │ ...   │  │
│  │  2 │ Bob Smith      │  $12,891  │  │   41 │ income   │   $320 │ ...   │  │
│  │  3 │ Carol Johnson  │   $3,102  │  │   40 │ signup   │   $500 │ ...   │  │
│  │  4 │ David Williams │   $8,445  │  │   39 │ expense  │    $85 │ ...   │  │
│  └────┴────────────────┴───────────┘  └──────┴──────────┴────────┴───────┘  │
│                                                                             │
│  ┌─ How It Works ─────────────────────────────────────────────────────────┐ │
│  │  This demo uses MnemonicDB's temporal features. Every change is        │ │
│  │  recorded with a transaction ID. Use the slider to travel back in      │ │
│  │  time and see exactly what the database looked like at any step.       │ │
│  └────────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Historical Mode Indicator

When viewing historical data (not live), show:
- Amber border around the entire demo
- "Viewing Step X (Historical)" badge
- "Resume Live" button becomes prominent

---

## Implementation Phases

### Phase 1: React Migration
- [ ] Update dependencies (remove preact, add react)
- [ ] Update astro.config.mjs
- [ ] Update tsconfig.json
- [ ] Migrate CodeRunner.tsx to React
- [ ] Migrate ResultsTable.tsx to React
- [ ] Test existing docs functionality

### Phase 2: Install TanStack & Components
- [ ] Install @tanstack/react-query
- [ ] Install @tanstack/react-table
- [ ] Install rc-slider
- [ ] Install lucide-react
- [ ] Create QueryClientProvider wrapper

### Phase 3: Bank Demo Schema
- [ ] Create bankSchema.ts with SQL
- [ ] Create simulation_steps table
- [ ] Create useBankQueries.ts hooks
- [ ] Test schema initialization

### Phase 4: Core Components
- [ ] Create types.ts
- [ ] Create BankDemo.tsx (provider wrapper)
- [ ] Create BankDemoContent.tsx (main logic)
- [ ] Create AccountsGrid.tsx with TanStack Table
- [ ] Create TransactionsGrid.tsx with TanStack Table
- [ ] Create AggregateStats.tsx

### Phase 5: Simulation & Time Controls
- [ ] Create SimulationEngine.ts
- [ ] Implement step execution with DB mutations
- [ ] Create TimeControls.tsx with rc-slider
- [ ] Wire up play/pause/step controls
- [ ] Implement time-travel with as_of_tx

### Phase 6: Polish & Documentation
- [ ] Create bank-demo.mdx page
- [ ] Style components with synthwave theme
- [ ] Add explanatory text
- [ ] Handle edge cases and errors
- [ ] Test full demo flow

---

## Files to Create/Modify

### Modified Files (Preact → React Migration)
1. `package.json` - Update dependencies
2. `astro.config.mjs` - Switch integration
3. `tsconfig.json` - Update JSX settings
4. `src/components/CodeRunner.tsx` - React syntax
5. `src/components/ResultsTable.tsx` - React syntax

### New Files
1. `src/components/BankDemo/BankDemo.tsx`
2. `src/components/BankDemo/BankDemoContent.tsx`
3. `src/components/BankDemo/TimeControls.tsx`
4. `src/components/BankDemo/AccountsGrid.tsx`
5. `src/components/BankDemo/TransactionsGrid.tsx`
6. `src/components/BankDemo/AggregateStats.tsx`
7. `src/components/BankDemo/SimulationEngine.ts`
8. `src/components/BankDemo/useBankQueries.ts`
9. `src/components/BankDemo/bankSchema.ts`
10. `src/components/BankDemo/types.ts`
11. `src/content/docs/examples/bank-demo.mdx`

---

## Success Criteria

1. ✅ Docs site works with React (all existing demos functional)
2. ✅ Simulation runs automatically at 1 step/second
3. ✅ TanStack Table displays accounts and transactions
4. ✅ TanStack Query manages data fetching and caching
5. ✅ Aggregate stats update when data changes
6. ✅ rc-slider allows scrubbing through history
7. ✅ Time-travel shows exact database state at each step
8. ✅ "Resume Live" returns to current state
9. ✅ Play/pause and step buttons work correctly
10. ✅ Visual design matches synthwave theme
11. ✅ Demo is documented and explained

---

## Sources

- [TanStack Table Documentation](https://tanstack.com/table/latest)
- [TanStack Query Documentation](https://tanstack.com/query/latest)
- [rc-slider NPM](https://www.npmjs.com/package/rc-slider)
- [Astro React Integration](https://docs.astro.build/en/guides/integrations-guide/react/)
- [PGLite Live Queries](https://pglite.dev/docs/live-queries)
- [Lucide React Icons](https://lucide.dev/)
