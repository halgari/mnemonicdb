import type { FC } from 'react';
import Slider from 'rc-slider';
import { Play, Pause, SkipBack, SkipForward, Radio } from 'lucide-react';
import 'rc-slider/assets/index.css';

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

export const TimeControls: FC<TimeControlsProps> = ({
  simulationStep,
  viewingStep,
  isPlaying,
  isLive,
  onPlay,
  onPause,
  onStepForward,
  onStepBack,
  onSeek,
  onResumeLive,
}) => {
  const buttonStyle: React.CSSProperties = {
    display: 'flex',
    alignItems: 'center',
    justifyContent: 'center',
    width: '36px',
    height: '36px',
    borderRadius: '8px',
    border: '1px solid rgba(160, 160, 192, 0.3)',
    background: 'rgba(26, 26, 46, 0.8)',
    color: '#e0e0ff',
    cursor: 'pointer',
    transition: 'all 0.2s ease',
    padding: 0,
    margin: 0,
  };

  const playButtonStyle: React.CSSProperties = {
    ...buttonStyle,
    background: isPlaying
      ? 'rgba(255, 42, 109, 0.2)'
      : 'linear-gradient(135deg, #ff2a6d, #d300c5)',
    border: isPlaying ? '1px solid #ff2a6d' : 'none',
  };

  const liveButtonStyle: React.CSSProperties = {
    display: 'flex',
    alignItems: 'center',
    gap: '6px',
    padding: '8px 16px',
    borderRadius: '8px',
    border: 'none',
    background: isLive
      ? 'rgba(255, 68, 68, 0.2)'
      : 'linear-gradient(135deg, #ff4444, #cc0000)',
    color: isLive ? '#ff6666' : 'white',
    cursor: isLive ? 'default' : 'pointer',
    fontWeight: 600,
    fontSize: '0.85rem',
    transition: 'all 0.2s ease',
    margin: 0,
  };

  return (
    <div style={{
      display: 'flex',
      flexDirection: 'column',
      gap: '12px',
      padding: '16px 20px',
      background: 'rgba(22, 22, 42, 0.6)',
      borderRadius: '12px',
      border: '1px solid rgba(160, 160, 192, 0.2)',
    }}>
      <div style={{
        display: 'flex',
        alignItems: 'center',
        gap: '16px',
      }}>
        {/* Control buttons - uniform spacing */}
        <div style={{ display: 'flex', gap: '4px', alignItems: 'center' }}>
          <button
            onClick={onStepBack}
            disabled={viewingStep <= 0}
            style={{
              ...buttonStyle,
              opacity: viewingStep <= 0 ? 0.4 : 1,
              cursor: viewingStep <= 0 ? 'not-allowed' : 'pointer',
            }}
            title="Step Back"
          >
            <SkipBack size={18} />
          </button>

          <button
            onClick={isPlaying ? onPause : onPlay}
            style={playButtonStyle}
            title={isPlaying ? 'Pause' : 'Play'}
          >
            {isPlaying ? <Pause size={18} /> : <Play size={18} />}
          </button>

          <button
            onClick={onStepForward}
            style={buttonStyle}
            title="Step Forward"
          >
            <SkipForward size={18} />
          </button>
        </div>

        {/* Slider */}
        <div style={{ flex: 1, padding: '0 8px' }}>
          <Slider
            min={0}
            max={Math.max(simulationStep, 1)}
            value={viewingStep}
            onChange={(value) => onSeek(value as number)}
            styles={{
              track: { backgroundColor: '#ff2a6d', height: 6 },
              rail: { backgroundColor: '#3a3a5c', height: 6 },
              handle: {
                borderColor: '#ff2a6d',
                backgroundColor: '#d300c5',
                width: 16,
                height: 16,
                marginTop: -5,
                opacity: 1,
              },
            }}
          />
        </div>

        {/* Live button */}
        <button
          onClick={onResumeLive}
          disabled={isLive}
          style={liveButtonStyle}
          title={isLive ? 'Viewing live' : 'Resume live view'}
        >
          <Radio
            size={14}
            style={{
              color: isLive ? '#ff4444' : 'white',
              animation: isLive ? 'pulse 1.5s ease-in-out infinite' : 'none',
            }}
          />
          LIVE
        </button>
      </div>

      {/* Step indicator */}
      <div style={{
        display: 'flex',
        justifyContent: 'center',
        alignItems: 'center',
        gap: '8px',
        color: '#a0a0c0',
        fontSize: '0.85rem',
      }}>
        <span>
          {isLive ? (
            <>Viewing <strong style={{ color: '#ff4444' }}>Live</strong> &middot; </>
          ) : (
            <span style={{ color: '#ffd700' }}>Historical View &middot; </span>
          )}
          Step <strong style={{ color: '#e0e0ff' }}>{viewingStep}</strong> of{' '}
          <strong style={{ color: '#e0e0ff' }}>{simulationStep}</strong>
        </span>
        <span style={{ color: '#606080' }}>&middot;</span>
        <span>250ms/step</span>
      </div>
    </div>
  );
};

export default TimeControls;
