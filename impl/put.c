/*
 * tkvdb
 *
 * Copyright (c) 2016-2018, Vladimir Misyurov
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/* add key-value pair to memory transaction */
static TKVDB_RES
TKVDB_IMPL_PUT(tkvdb_tr *trns, const tkvdb_datum *key, const tkvdb_datum *val)
{
	const unsigned char *sym;  /* pointer to current symbol in key */
	TKVDB_MEMNODE_TYPE *node;  /* current node */
	size_t pi;                 /* prefix index */

	/* pointer to data of node, it can be different for leaf and ordinary
	 nodes */
#ifdef TKVDB_PARAMS_ALIGN_VAL
	uint8_t *prefix;
	uint8_t *val_ptr;
	uint8_t *meta_ptr;
#else
	uint8_t *prefix_val_meta;
#endif

	tkvdb_tr_data *tr = trns->data;

	if (!tr->started) {
		return TKVDB_NOT_STARTED;
	}

	/* new root */
	if (tr->root == NULL) {
		if (tr->db && (tr->db->info.filesize > 0)) {
			/* we have underlying non-empty db file */
			TKVDB_EXEC( TKVDB_IMPL_NODE_READ(trns,
				tr->db->info.footer.root_off,
				((TKVDB_MEMNODE_TYPE **)&(tr->root))) );
		} else {
			tr->root = TKVDB_IMPL_NODE_NEW(trns,
				TKVDB_NODE_VAL | TKVDB_NODE_LEAF,
				key->size, key->data, val->size, val->data);
			if (!tr->root) {
				return TKVDB_ENOMEM;
			}
			return TKVDB_OK;
		}
	}

	sym = key->data;
	node = tr->root;

next_node:
	TKVDB_SKIP_RNODES(node);

#ifdef TKVDB_PARAMS_ALIGN_VAL
	/* align val */
	if (node->c.type & TKVDB_NODE_LEAF) {
		prefix = ((TKVDB_MEMNODE_TYPE_LEAF *)node)->prefix_val_meta;
	} else {
		prefix = node->prefix_val_meta;
	}

	val_ptr = prefix + node->c.prefix_size;
	val_ptr = (uint8_t *)((((uintptr_t)val_ptr + tr->valign - 1)
		/ tr->valign) * tr->valign);
	meta_ptr = val_ptr + node->c.val_size;
	meta_ptr = (uint8_t *)((((uintptr_t)meta_ptr + tr->valign - 1)
		/ tr->valign) * tr->valign);
#else
	if (node->c.type & TKVDB_NODE_LEAF) {
		prefix_val_meta =
			((TKVDB_MEMNODE_TYPE_LEAF *)node)->prefix_val_meta;
	} else {
		prefix_val_meta = node->prefix_val_meta;
	}
#endif
	pi = 0;

next_byte:

/* end of key
  Here we have two cases:
  [p][r][e][f][i][x] - prefix
  [p][r][e] - new key

  or exact match:
  [p][r][e][f][i][x] - prefix
  [p][r][e][f][i][x] - new key
*/
	if (sym >= ((unsigned char *)key->data + key->size)) {
		TKVDB_MEMNODE_TYPE *newroot, *subnode_rest;

		if (pi == node->c.prefix_size) {
			/* exact match */
			if ((node->c.val_size == val->size)
				&& (val->size != 0)) {
				/* same value size, so copy new value and
					return */
#ifdef TKVDB_PARAMS_ALIGN_VAL
				memcpy(val_ptr, val->data, val->size);
#else
				memcpy(prefix_val_meta + node->c.prefix_size,
					val->data, val->size);
#endif
				return TKVDB_OK;
			}

#ifdef TKVDB_PARAMS_ALIGN_VAL
			newroot = TKVDB_IMPL_NODE_NEW(trns,
				node->c.type | TKVDB_NODE_VAL,
				pi, prefix,
				val->size, val->data);
#else
			newroot = TKVDB_IMPL_NODE_NEW(trns,
				node->c.type | TKVDB_NODE_VAL,
				pi, prefix_val_meta,
				val->size, val->data);
#endif
			if (!newroot) return TKVDB_ENOMEM;

			TKVDB_IMPL_CLONE_SUBNODES(newroot, node);

			TKVDB_REPLACE_NODE(node, newroot);

			return TKVDB_OK;
		}

/* split node with prefix
  [p][r][e][f][i][x] - prefix
  [p][r][e] - new key

  becomes
  [p][r][e] - new root
  next['f'] => [i][x] - tail
*/
#ifdef TKVDB_PARAMS_ALIGN_VAL
		newroot = TKVDB_IMPL_NODE_NEW(trns, TKVDB_NODE_VAL, pi,
			prefix, val->size, val->data);
#else
		newroot = TKVDB_IMPL_NODE_NEW(trns, TKVDB_NODE_VAL, pi,
			prefix_val_meta,
			val->size, val->data);
#endif
		if (!newroot) return TKVDB_ENOMEM;

#ifdef TKVDB_PARAMS_ALIGN_VAL
		subnode_rest = TKVDB_IMPL_NODE_NEW(trns,
			node->c.type & (~TKVDB_NODE_LEAF),
			node->c.prefix_size - pi - 1,
			prefix + pi + 1,
			node->c.val_size,
			val_ptr);
#else
		subnode_rest = TKVDB_IMPL_NODE_NEW(trns,
			node->c.type & (~TKVDB_NODE_LEAF),
			node->c.prefix_size - pi - 1,
			prefix_val_meta + pi + 1,
			node->c.val_size,
			prefix_val_meta + node->c.prefix_size);
#endif
		if (!subnode_rest) {
			if (tr->params.tr_buf_dynalloc) {
				free(newroot);
			}
			return TKVDB_ENOMEM;
		}
		TKVDB_IMPL_CLONE_SUBNODES(subnode_rest, node);

#ifdef TKVDB_PARAMS_ALIGN_VAL
		newroot->next[prefix[pi]] = subnode_rest;
#else
		newroot->next[prefix_val_meta[pi]] = subnode_rest;
#endif

		TKVDB_REPLACE_NODE(node, newroot);

		return TKVDB_OK;
	}

/* end of prefix
  [p][r][e][f][i][x] - old prefix
  [p][r][e][f][i][x][n][e][w]- new prefix

  so we hold old node and change only pointer to next
  [p][r][e][f][i][x]
  next['n'] => [e][w] - tail
*/
	if (pi >= node->c.prefix_size) {
		if (node->c.type & TKVDB_NODE_LEAF) {
			/* create 2 nodes */
			TKVDB_MEMNODE_TYPE *newroot, *subnode_rest;
#ifdef TKVDB_PARAMS_ALIGN_VAL
			newroot = TKVDB_IMPL_NODE_NEW(trns,
				node->c.type & (~TKVDB_NODE_LEAF),
				node->c.prefix_size,
				prefix,
				node->c.val_size,
				val_ptr);
#else
			newroot = TKVDB_IMPL_NODE_NEW(trns,
				node->c.type & (~TKVDB_NODE_LEAF),
				node->c.prefix_size,
				prefix_val_meta,
				node->c.val_size,
				prefix_val_meta + node->c.prefix_size);
#endif
			if (!newroot) {
				if (tr->params.tr_buf_dynalloc) {
					free(newroot);
				}
				return TKVDB_ENOMEM;
			}

			subnode_rest = TKVDB_IMPL_NODE_NEW(trns,
				TKVDB_NODE_VAL | TKVDB_NODE_LEAF,
				key->size -
					(sym - (unsigned char *)key->data) - 1,
				sym + 1,
				val->size, val->data);
			if (!subnode_rest) return TKVDB_ENOMEM;

			newroot->next[*sym] = subnode_rest;

			TKVDB_REPLACE_NODE(node, newroot);

			return TKVDB_OK;
		} else if (node->next[*sym] != NULL) {
			/* continue with next node */
			node = node->next[*sym];
			sym++;
			goto next_node;
		} else if (tr->db && (node->fnext[*sym] != 0)) {
			TKVDB_MEMNODE_TYPE *tmp;

			/* load subnode from disk */
			TKVDB_EXEC( TKVDB_IMPL_NODE_READ(trns,
				node->fnext[*sym], &tmp) );

			node->next[*sym] = tmp;
			node = tmp;
			sym++;
			goto next_node;
		} else {
			TKVDB_MEMNODE_TYPE *tmp;

			/* allocate tail */
			tmp = TKVDB_IMPL_NODE_NEW(trns,
				TKVDB_NODE_VAL | TKVDB_NODE_LEAF,
				key->size -
					(sym - (unsigned char *)key->data) - 1,
				sym + 1,
				val->size, val->data);
			if (!tmp) return TKVDB_ENOMEM;

			node->next[*sym] = tmp;
			return TKVDB_OK;
		}
	}

/* node prefix don't match with corresponding part of key
  [p][r][e][f][i][x] - old prefix
  [p][r][e][p][a][r][e]- new prefix

  [p][r][e] - new root
  next['f'] => [i][x] - tail from old prefix
  next['p'] => [a][r][e] - tail from new prefix
*/
#ifdef TKVDB_PARAMS_ALIGN_VAL
	if (prefix[pi] != *sym) {
#else
	if (prefix_val_meta[pi] != *sym) {
#endif
		TKVDB_MEMNODE_TYPE *newroot, *subnode_rest, *subnode_key;

		/* split current node into 3 subnodes */
#ifdef TKVDB_PARAMS_ALIGN_VAL
		newroot = TKVDB_IMPL_NODE_NEW(trns, 0, pi,
			prefix, 0, NULL);
#else
		newroot = TKVDB_IMPL_NODE_NEW(trns, 0, pi,
			prefix_val_meta, 0, NULL);
#endif
		if (!newroot) return TKVDB_ENOMEM;

		/* rest of prefix (skip current symbol) */
#ifdef TKVDB_PARAMS_ALIGN_VAL
		subnode_rest = TKVDB_IMPL_NODE_NEW(trns,
			node->c.type & (~TKVDB_NODE_LEAF),
			node->c.prefix_size - pi - 1,
			prefix + pi + 1,
			node->c.val_size,
			val_ptr);
#else
		subnode_rest = TKVDB_IMPL_NODE_NEW(trns,
			node->c.type & (~TKVDB_NODE_LEAF),
			node->c.prefix_size - pi - 1,
			prefix_val_meta + pi + 1,
			node->c.val_size,
			prefix_val_meta + node->c.prefix_size);
#endif
		if (!subnode_rest) {
			if (tr->params.tr_buf_dynalloc) {
				free(newroot);
			}
			return TKVDB_ENOMEM;
		}
		TKVDB_IMPL_CLONE_SUBNODES(subnode_rest, node);

		/* rest of key */
		subnode_key = TKVDB_IMPL_NODE_NEW(trns,
			TKVDB_NODE_VAL | TKVDB_NODE_LEAF,
			key->size -
				(sym - (unsigned char *)key->data) - 1,
			sym + 1,
			val->size, val->data);
		if (!subnode_key) {
			if (tr->params.tr_buf_dynalloc) {
				free(subnode_rest);
				free(newroot);
			}
			return TKVDB_ENOMEM;
		}

#ifdef TKVDB_PARAMS_ALIGN_VAL
		newroot->next[prefix[pi]] = subnode_rest;
#else
		newroot->next[prefix_val_meta[pi]] = subnode_rest;
#endif
		newroot->next[*sym] = subnode_key;

		TKVDB_REPLACE_NODE(node, newroot);

		return TKVDB_OK;
	}

	sym++;
	pi++;
	goto next_byte;

	return TKVDB_OK;
}

