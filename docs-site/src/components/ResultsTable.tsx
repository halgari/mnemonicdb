import type { FC } from 'react';

interface ResultsTableProps {
  rows: Record<string, unknown>[];
  fields: { name: string }[];
}

function formatValue(value: unknown): string {
  if (value === null) return 'NULL';
  if (value === undefined) return '';
  if (typeof value === 'object') return JSON.stringify(value);
  if (typeof value === 'bigint') return value.toString();
  return String(value);
}

export const ResultsTable: FC<ResultsTableProps> = ({
  rows,
  fields,
}) => {
  if (fields.length === 0) {
    return (
      <div className="results-message">
        Query executed successfully (no results to display)
      </div>
    );
  }

  if (rows.length === 0) {
    return (
      <div className="results-message">
        No rows returned
      </div>
    );
  }

  return (
    <div className="results-container fade-in">
      <table className="results-table">
        <thead>
          <tr>
            {fields.map((field) => (
              <th key={field.name}>{field.name}</th>
            ))}
          </tr>
        </thead>
        <tbody>
          {rows.map((row, rowIndex) => (
            <tr key={rowIndex}>
              {fields.map((field) => (
                <td key={field.name}>
                  <code className="result-value">{formatValue(row[field.name])}</code>
                </td>
              ))}
            </tr>
          ))}
        </tbody>
      </table>
      <div className="results-count">
        {rows.length} row{rows.length !== 1 ? 's' : ''} returned
      </div>
    </div>
  );
};

export default ResultsTable;
