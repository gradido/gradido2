import { getDecayRespiteCent, getDecayStartTime } from '@gradido/shared-native'

/**
 * The moment decay begins. Before it, a balance does not shrink at all -- the chain
 * predates the rule, and applying it retroactively would rewrite settled history.
 *
 * Read from the native library rather than repeated here, so TypeScript and the C
 * validation cannot disagree about where the line falls.
 */
export const DECAY_START_TIME = getDecayStartTime()

/**
 * The tolerance buffer for balance validation and decay calculations, expressed in GradidoCent.
 *
 * This constant accounts for rounding errors, timestamp discrepancies (e.g., Hedera consensus
 * delay), and natural imprecision in continuous decay calculations. It ensures that
 * micro-transactions are not incorrectly rejected due to mathematical drift, reflecting
 * Gradido's principle of generosity.
 *
 * 100 GradidoCent = 0.01 GDD
 */
export const DECAY_RESPITE_CENT = getDecayRespiteCent()
