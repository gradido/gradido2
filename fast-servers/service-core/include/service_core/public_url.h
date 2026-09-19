/*
 * Whether a URL names a host on the public internet: a domain, or an address that is routed there.
 *
 * `setup` decides with it whether the dht node starts PUBLIC or PRIVATE. The reference is
 * `packages/service-core/src/utils/publicUrl.ts`, which has the full list of what counts as
 * private; contracts/test-vectors/public-url.json holds both to the same answers. The host is
 * judged as it was written: `http://2130706433` is a number, not 127.0.0.1 -- and not public.
 */
#ifndef SERVICE_CORE_PUBLIC_URL_H
#define SERVICE_CORE_PUBLIC_URL_H

/** 1 when @p url names a public domain or address, 0 for everything else and for NULL. */
int sc_url_is_public(const char *url);

#endif /* SERVICE_CORE_PUBLIC_URL_H */
