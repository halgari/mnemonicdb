// Bank Simulation Engine
import type { Account, SimEvent } from './types';

// Seeded random number generator for reproducible simulations
function createSeededRandom(seed: number): () => number {
  let state = seed;
  return () => {
    state = (state * 1103515245 + 12345) & 0x7fffffff;
    return state / 0x7fffffff;
  };
}

const FIRST_NAMES = [
  'Alice', 'Bob', 'Carol', 'David', 'Emma', 'Frank', 'Grace', 'Henry',
  'Ivy', 'Jack', 'Kate', 'Leo', 'Maya', 'Noah', 'Olivia', 'Paul',
  'Quinn', 'Ruby', 'Sam', 'Tara', 'Uma', 'Victor', 'Wendy', 'Xavier'
];

const LAST_NAMES = [
  'Chen', 'Smith', 'Johnson', 'Williams', 'Brown', 'Jones', 'Garcia',
  'Miller', 'Davis', 'Rodriguez', 'Martinez', 'Wilson', 'Anderson',
  'Taylor', 'Thomas', 'Moore', 'Jackson', 'White', 'Harris', 'Clark'
];

const INCOME_DESCRIPTIONS = [
  'Salary deposit', 'Freelance payment', 'Bonus', 'Refund',
  'Investment return', 'Gift received', 'Side gig income'
];

const EXPENSE_DESCRIPTIONS = [
  'Groceries', 'Utilities', 'Subscription', 'Dining out',
  'Transportation', 'Entertainment', 'Shopping', 'Healthcare'
];

const TRANSFER_DESCRIPTIONS = [
  'Rent payment', 'Loan repayment', 'Split bill', 'Gift',
  'Shared expense', 'Payment for services', 'Reimbursement'
];

function pickRandom<T>(arr: T[], rng: () => number): T {
  return arr[Math.floor(rng() * arr.length)];
}

function generateName(rng: () => number): string {
  const first = pickRandom(FIRST_NAMES, rng);
  const last = pickRandom(LAST_NAMES, rng);
  return `${first} ${last}`;
}

const MAX_EVENTS_PER_STEP = 10;

export function generateEvents(accounts: Account[], step: number): SimEvent[] {
  const events: SimEvent[] = [];
  const rng = createSeededRandom(step * 31337);

  // Always create at least one account in the first few steps
  // 30% chance of new signup (increased from 15%)
  const shouldSignup = accounts.length === 0 ||
                       (accounts.length < 3 && step < 5) ||
                       rng() < 0.30;

  if (shouldSignup) {
    events.push({
      type: 'signup',
      name: generateName(rng),
      initialBalance: Math.floor(rng() * 900) + 100, // $100-$1000
    });
  }

  // Each existing account may do something
  for (const account of accounts) {
    // Stop if we've hit the cap
    if (events.length >= MAX_EVENTS_PER_STEP) break;

    const roll = rng();

    if (roll < 0.20) {
      // 20% - nothing happens
      continue;
    } else if (roll < 0.45) {
      // 25% - income ($50-$500)
      events.push({
        type: 'income',
        accountId: account.id,
        amount: Math.floor(rng() * 450) + 50,
        description: pickRandom(INCOME_DESCRIPTIONS, rng),
      });
    } else if (roll < 0.70 && account.balance > 50) {
      // 25% - expense (up to 30% of balance, max $200)
      const maxExpense = Math.min(account.balance * 0.3, 200);
      if (maxExpense > 10) {
        events.push({
          type: 'expense',
          accountId: account.id,
          amount: Math.floor(rng() * (maxExpense - 10)) + 10,
          description: pickRandom(EXPENSE_DESCRIPTIONS, rng),
        });
      }
    } else if (accounts.length > 1 && account.balance > 100) {
      // 30% - transfer to random other account
      const others = accounts.filter(a => a.id !== account.id);
      if (others.length > 0) {
        const target = pickRandom(others, rng);
        const maxTransfer = Math.min(account.balance * 0.25, 150);
        if (maxTransfer > 20) {
          events.push({
            type: 'transfer',
            fromId: account.id,
            toId: target.id,
            amount: Math.floor(rng() * (maxTransfer - 20)) + 20,
            description: pickRandom(TRANSFER_DESCRIPTIONS, rng),
          });
        }
      }
    }
  }

  return events;
}

export function generateEventSQL(event: SimEvent, step: number): string[] {
  const statements: string[] = [];

  switch (event.type) {
    case 'signup':
      // Create new account and record the signup transaction
      statements.push(`
        INSERT INTO accounts (name, balance, last_seen)
        VALUES ('${event.name.replace(/'/g, "''")}', ${event.initialBalance}, ${step})
        RETURNING id
      `);
      break;

    case 'income':
      statements.push(`
        UPDATE accounts
        SET balance = balance + ${event.amount}, last_seen = ${step}
        WHERE id = ${event.accountId}
      `);
      statements.push(`
        INSERT INTO bank_transactions (step, type, amount, to_account, description)
        VALUES (${step}, 'income', ${event.amount}, ${event.accountId}, '${event.description.replace(/'/g, "''")}')
      `);
      break;

    case 'expense':
      statements.push(`
        UPDATE accounts
        SET balance = balance - ${event.amount}, last_seen = ${step}
        WHERE id = ${event.accountId}
      `);
      statements.push(`
        INSERT INTO bank_transactions (step, type, amount, from_account, description)
        VALUES (${step}, 'expense', ${event.amount}, ${event.accountId}, '${event.description.replace(/'/g, "''")}')
      `);
      break;

    case 'transfer':
      statements.push(`
        UPDATE accounts
        SET balance = balance - ${event.amount}, last_seen = ${step}
        WHERE id = ${event.fromId}
      `);
      statements.push(`
        UPDATE accounts
        SET balance = balance + ${event.amount}, last_seen = ${step}
        WHERE id = ${event.toId}
      `);
      statements.push(`
        INSERT INTO bank_transactions (step, type, amount, from_account, to_account, description)
        VALUES (${step}, 'transfer', ${event.amount}, ${event.fromId}, ${event.toId}, '${event.description.replace(/'/g, "''")}')
      `);
      break;
  }

  return statements;
}

// Special handling for signup which needs the new account ID
export function generateSignupSQL(name: string, initialBalance: number, step: number): string {
  return `
    WITH new_account AS (
      INSERT INTO accounts (name, balance, last_seen)
      VALUES ('${name.replace(/'/g, "''")}', ${initialBalance}, ${step})
      RETURNING id
    )
    INSERT INTO bank_transactions (step, type, amount, to_account, description)
    SELECT ${step}, 'signup', ${initialBalance}, id, 'New account created'
    FROM new_account
  `;
}
