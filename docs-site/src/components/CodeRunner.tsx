import { useState, useEffect, useRef, type FC, type ChangeEvent } from 'react';
import { executeQuery, resetDatabase, getDatabase } from '../lib/pglite-loader';
import { ResultsTable } from './ResultsTable';

interface CodeRunnerProps {
  initialCode: string;
  title?: string;
}

interface QueryResult {
  rows: Record<string, unknown>[];
  fields: { name: string }[];
}

type Status = 'idle' | 'initializing' | 'running' | 'success' | 'error';

// SQL syntax highlighter
function highlightSQL(code: string): string {
  const keywords = [
    'SELECT', 'FROM', 'WHERE', 'INSERT', 'INTO', 'VALUES', 'UPDATE', 'SET',
    'DELETE', 'CREATE', 'TABLE', 'DROP', 'ALTER', 'INDEX', 'VIEW', 'AS',
    'JOIN', 'LEFT', 'RIGHT', 'INNER', 'OUTER', 'ON', 'AND', 'OR', 'NOT',
    'NULL', 'IS', 'IN', 'LIKE', 'BETWEEN', 'ORDER', 'BY', 'ASC', 'DESC',
    'GROUP', 'HAVING', 'LIMIT', 'OFFSET', 'DISTINCT', 'COUNT', 'SUM', 'AVG',
    'MAX', 'MIN', 'CASE', 'WHEN', 'THEN', 'ELSE', 'END', 'CAST', 'COALESCE',
    'PRIMARY', 'KEY', 'FOREIGN', 'REFERENCES', 'UNIQUE', 'CHECK', 'DEFAULT',
    'CONSTRAINT', 'IF', 'EXISTS', 'TRUE', 'FALSE', 'WITH', 'RETURNING',
    'SERIAL', 'INT', 'INTEGER', 'BIGINT', 'TEXT', 'VARCHAR', 'BOOLEAN',
    'TIMESTAMP', 'TIMESTAMPTZ', 'DATE', 'TIME', 'JSONB', 'JSON', 'UUID',
    'CALL', 'PROCEDURE', 'FUNCTION', 'ARRAY'
  ];

  let result = code;

  // Escape HTML
  result = result.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

  // Highlight strings (single quotes)
  result = result.replace(/'([^']*)'/g, '<span class="sql-string">\'$1\'</span>');

  // Highlight numbers
  result = result.replace(/\b(\d+)\b/g, '<span class="sql-number">$1</span>');

  // Highlight keywords (case-insensitive)
  const keywordPattern = new RegExp(`\\b(${keywords.join('|')})\\b`, 'gi');
  result = result.replace(keywordPattern, '<span class="sql-keyword">$1</span>');

  // Highlight comments
  result = result.replace(/--(.*?)$/gm, '<span class="sql-comment">--$1</span>');

  return result;
}

export const CodeRunner: FC<CodeRunnerProps> = ({
  initialCode,
  title = 'Live Example',
}) => {
  const [code, setCode] = useState(initialCode);
  const [status, setStatus] = useState<Status>('idle');
  const [results, setResults] = useState<QueryResult[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [dbReady, setDbReady] = useState(false);
  const textareaRef = useRef<HTMLTextAreaElement>(null);
  const highlightRef = useRef<HTMLDivElement>(null);

  // Sync scroll between textarea and highlight overlay
  const syncScroll = () => {
    if (textareaRef.current && highlightRef.current) {
      highlightRef.current.scrollTop = textareaRef.current.scrollTop;
      highlightRef.current.scrollLeft = textareaRef.current.scrollLeft;
    }
  };

  // Initialize database on mount
  useEffect(() => {
    let cancelled = false;
    setStatus('initializing');

    const initDb = async () => {
      try {
        await getDatabase();
        if (!cancelled) {
          setDbReady(true);
          setStatus('idle');
        }
      } catch (err) {
        console.error('PGLite init error:', err);
        if (!cancelled) {
          setError(`Failed to initialize database: ${err instanceof Error ? err.message : String(err)}`);
          setStatus('error');
        }
      }
    };

    initDb();

    return () => {
      cancelled = true;
    };
  }, []);

  const handleRun = async () => {
    if (!dbReady) return;

    setStatus('running');
    setError(null);
    setResults([]);

    try {
      const db = await getDatabase();

      // Split into statements and execute
      const statements = code
        .split(';')
        .map((s) => s.trim())
        .filter((s) => s.length > 0);

      const allResults: QueryResult[] = [];

      for (const stmt of statements) {
        // Strip SQL comments to detect query type
        const stmtNoComments = stmt
          .replace(/--.*$/gm, '')  // Remove line comments
          .replace(/\/\*[\s\S]*?\*\//g, '')  // Remove block comments
          .trim();
        const upperStmt = stmtNoComments.toUpperCase();
        const isQuery = upperStmt.startsWith('SELECT') ||
                       upperStmt.includes('RETURNING');

        console.log('Executing statement:', stmt);
        console.log('Statement without comments:', stmtNoComments);
        console.log('Is query:', isQuery);

        if (isQuery) {
          const result = await executeQuery(stmt);
          console.log('Query result:', result);
          console.log('Fields length:', result?.fields?.length);
          console.log('Rows length:', result?.rows?.length);
          // Add result if we have either fields or rows
          if (result && (result.fields?.length > 0 || result.rows?.length > 0)) {
            allResults.push(result);
          }
        } else {
          await db.exec(stmt);
        }
      }

      setResults(allResults);
      setStatus('success');
    } catch (err) {
      console.error('Query error:', err);
      setError(err instanceof Error ? err.message : String(err));
      setStatus('error');
    }
  };

  const handleReset = async () => {
    setStatus('initializing');
    setResults([]);
    setError(null);
    setCode(initialCode);

    try {
      await resetDatabase();
      setStatus('idle');
    } catch (err) {
      setError(`Failed to reset database: ${err instanceof Error ? err.message : String(err)}`);
      setStatus('error');
    }
  };

  const handleCodeChange = (e: ChangeEvent<HTMLTextAreaElement>) => {
    setCode(e.target.value);
  };

  const buttonBaseStyle: React.CSSProperties = {
    padding: '6px 14px',
    borderRadius: '4px',
    fontWeight: '600',
    fontSize: '0.75rem',
    lineHeight: '1',
    cursor: 'pointer',
    textTransform: 'uppercase',
    letterSpacing: '0.05em',
    verticalAlign: 'middle',
    margin: '0',
  };

  const buttonStyle: React.CSSProperties = {
    ...buttonBaseStyle,
    background: 'linear-gradient(135deg, #ff2a6d, #d300c5)',
    color: 'white',
    border: 'none',
  };

  const resetButtonStyle: React.CSSProperties = {
    ...buttonBaseStyle,
    background: 'transparent',
    border: '1px solid rgba(160, 160, 192, 0.4)',
    color: '#a0a0c0',
  };

  return (
    <div className="live-example">
      <div className="live-example-header">
        <span className="live-example-title">{title}</span>
        {status === 'initializing' && (
          <span className="header-status muted">Initializing PGLite...</span>
        )}
        {status === 'success' && (
          <span className="header-status success">Success</span>
        )}
      </div>

      <div className="live-example-editor">
        <div className="code-editor-container">
          <div
            ref={highlightRef}
            className="code-highlight"
            dangerouslySetInnerHTML={{ __html: highlightSQL(code) + '\n' }}
          />
          <textarea
            ref={textareaRef}
            value={code}
            onChange={handleCodeChange}
            onScroll={syncScroll}
            spellCheck={false}
            disabled={status === 'initializing' || status === 'running'}
          />
        </div>
      </div>

      <div className="live-example-actions">
        <button
          onClick={handleRun}
          disabled={!dbReady || status === 'running'}
          style={buttonStyle}
        >
          {status === 'running' ? 'Running...' : 'Run'}
        </button>
        <button
          onClick={handleReset}
          disabled={status === 'initializing' || status === 'running'}
          style={resetButtonStyle}
        >
          Reset DB
        </button>
      </div>

      <div
        className={`live-example-results ${status === 'error' ? 'error' : ''} ${
          status === 'initializing' || status === 'running' ? 'loading' : ''
        }`}
      >
        {(status === 'initializing' || status === 'running') && (
          <div className="loading-indicator">
            <div className="spinner" />
            <span>
              {status === 'initializing' ? 'Loading PGLite...' : 'Executing...'}
            </span>
          </div>
        )}

        {status === 'error' && (
          <div className="error-content">
            <strong>Error:</strong>
            <pre>{error}</pre>
          </div>
        )}

        {status === 'success' && results.length > 0 && (
          <div className="results-list">
            {results.map((result, idx) => (
              <div key={idx} className="result-set">
                {results.length > 1 && (
                  <div className="result-set-header">Result Set {idx + 1}</div>
                )}
                <ResultsTable rows={result.rows} fields={result.fields} />
              </div>
            ))}
          </div>
        )}

        {status === 'success' && results.length === 0 && (
          <div className="success-message fade-in">
            Statements executed successfully
          </div>
        )}

        {status === 'idle' && (
          <div className="idle-message">
            Click "Run" to execute the SQL
          </div>
        )}
      </div>
    </div>
  );
};

export default CodeRunner;
