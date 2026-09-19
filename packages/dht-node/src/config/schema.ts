import { portSchema, runtimeConfigSchema } from '@gradido/service-core'
import * as v from 'valibot'

/**
 * What the dht-node role reads -- the same variables as `fast-servers/service-core/src/config.c`.
 *
 * DHT_TOPIC, MASTER_SEED and DHT_DELEGATION are optional here and checked when the node starts,
 * where a missing one is the same log event the C path writes: `dht.topic_missing`,
 * `dht.master_seed_missing`, `dht.delegation_missing`. MASTER_SEED is a secret and has been
 * resolved by the time this schema sees it -- contracts/secrets.json.
 */
export const configSchema = v.object({
  ...runtimeConfigSchema.entries,
  DHT_PORT: v.optional(portSchema, '5000'),
  DHT_TOPIC: v.optional(v.string(), ''),
  MASTER_SEED: v.optional(v.string(), ''),
  DHT_DELEGATION: v.optional(v.string(), ''),
  DHT_REACHABILITY: v.optional(v.picklist(['public', 'private']), 'private'),
  /* DHT_BOOTSTRAP_DEFAULT_URL in contracts/const.json; empty joins through nobody -- the first
     community of a network, or one whose peers are added by hand. */
  DHT_BOOTSTRAP_URL: v.optional(v.string(), 'https://gdd.gradido.net'),
})

export type DhtNodeConfig = v.InferOutput<typeof configSchema>
