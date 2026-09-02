/*
 * The first-run conversation, which is the role's business rather than the domain's -- see
 * bc_context_open, which takes it as a callback for exactly that reason.
 */
#ifndef BACKEND_SETUP_H
#define BACKEND_SETUP_H

#include "backend_core/domain/community.h"

/**
 * Fills @p setup from a terminal, answering 1 when it did.
 *
 * 0 means there was nobody to ask: no terminal, or one that went away before the last answer. It
 * is not an error here -- the caller decides what a start without a community means.
 */
int backend_ask_for_home_community(bc_home_community_setup *setup);

#endif /* BACKEND_SETUP_H */
