import type { Duration } from './Duration'
import type { GradidoUnit } from './GradidoUnit'

/**
 * What a balance lost to decay over a stretch of time, and which stretch that was.
 *
 * `start`, `end` and `duration` describe the *effective* period, which is not always the
 * one that was asked for: decay never reaches back past DECAY_START_TIME. When the period
 * ends before decay begins at all, `start` and `end` stay null and `duration` is zero --
 * `balance` is then the untouched original and `decay` is 0 GDD.
 */
export interface Decay {
  /** The balance after decay was applied. */
  balance: GradidoUnit
  /** What was lost; negative, since it is `balance - original`. */
  decay: GradidoUnit
  /** Start of the effective period, or null when nothing decayed. */
  start: Date | null
  /** End of the effective period, or null when nothing decayed. */
  end: Date | null
  /** Length of the effective period. */
  duration: Duration | null
}
