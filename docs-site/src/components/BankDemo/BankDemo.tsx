import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { BankDemoContent } from './BankDemoContent';

// Create a client for this demo
const queryClient = new QueryClient({
  defaultOptions: {
    queries: {
      staleTime: 0,
      refetchOnWindowFocus: false,
    },
  },
});

const scrollbarStyles = `
  .bank-demo-scrollable::-webkit-scrollbar {
    width: 8px;
  }
  .bank-demo-scrollable::-webkit-scrollbar-track {
    background: rgba(22, 22, 42, 0.4);
    border-radius: 4px;
  }
  .bank-demo-scrollable::-webkit-scrollbar-thumb {
    background: rgba(160, 160, 192, 0.3);
    border-radius: 4px;
  }
  .bank-demo-scrollable::-webkit-scrollbar-thumb:hover {
    background: rgba(160, 160, 192, 0.5);
  }
  .bank-demo-scrollable {
    scrollbar-width: thin;
    scrollbar-color: rgba(160, 160, 192, 0.3) rgba(22, 22, 42, 0.4);
  }
  @keyframes pulse {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.5; }
  }
`;

export function BankDemo() {
  return (
    <QueryClientProvider client={queryClient}>
      <style>{scrollbarStyles}</style>
      <div style={{
        fontFamily: 'system-ui, -apple-system, sans-serif',
      }}>
        <BankDemoContent />
      </div>
    </QueryClientProvider>
  );
}

export default BankDemo;
