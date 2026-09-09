/*
 * The setup command's questions, which are the role's business rather than the domain's -- the
 * domain writes the row, this asks what goes in it. Reached from backend_setup() and from
 * nowhere else; a serving start does not come here at all, see bc_context_open.
 */
#ifndef BACKEND_SETUP_H
#define BACKEND_SETUP_H

#include "backend_core/domain/community.h"

/**
 * Fills @p setup from a terminal, answering 1 when it did.
 *
 * 0 means there was nobody to ask: no terminal, or one that went away before the last answer. It
 * is not an error here -- the caller decides what a setup without an answer means.
 */
int backend_ask_for_home_community(bc_home_community_setup *setup);

#endif /* BACKEND_SETUP_H */
