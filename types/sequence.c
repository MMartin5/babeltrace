/*
 * sequence.c
 *
 * BabelTrace - Sequence Type Converter
 *
 * Copyright 2010-2011 EfficiOS Inc. and Linux Foundation
 *
 * Author: Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 */

#include <babeltrace/compiler.h>
#include <babeltrace/format.h>
#include <inttypes.h>

static
struct definition *_sequence_definition_new(struct declaration *declaration,
					struct definition_scope *parent_scope,
					GQuark field_name, int index,
					const char *root_name);
static
void _sequence_definition_free(struct definition *definition);

int sequence_rw(struct stream_pos *pos, struct definition *definition)
{
	struct definition_sequence *sequence_definition =
		container_of(definition, struct definition_sequence, p);
	const struct declaration_sequence *sequence_declaration =
		sequence_definition->declaration;
	uint64_t len, oldlen, i;
	int ret;

	len = sequence_definition->length->value._unsigned;
	/*
	 * Yes, large sequences could be _painfully slow_ to parse due
	 * to memory allocation for each event read. At least, never
	 * shrink the sequence. Note: the sequence GArray len should
	 * never be used as indicator of the current sequence length.
	 * One should always look at the sequence->len->value._unsigned
	 * value for that.
	 */
	oldlen = sequence_definition->elems->len;
	if (oldlen < len)
		g_ptr_array_set_size(sequence_definition->elems, len);

	for (i = oldlen; i < len; i++) {
		struct definition **field;
		GString *str;
		GQuark name;

		str = g_string_new("");
		g_string_printf(str, "[%" PRIu64 "]", i);
		(void) g_string_free(str, TRUE);
		name = g_quark_from_string(str->str);

		field = (struct definition **) &g_ptr_array_index(sequence_definition->elems, i);
		*field = sequence_declaration->elem->definition_new(sequence_declaration->elem,
					  sequence_definition->p.scope,
					  name, i, NULL);
		ret = generic_rw(pos, *field);
		if (ret)
			return ret;
	}
	return 0;
}

static
void _sequence_declaration_free(struct declaration *declaration)
{
	struct declaration_sequence *sequence_declaration =
		container_of(declaration, struct declaration_sequence, p);

	free_declaration_scope(sequence_declaration->scope);
	g_array_free(sequence_declaration->length_name, TRUE);
	declaration_unref(sequence_declaration->elem);
	g_free(sequence_declaration);
}

struct declaration_sequence *
	sequence_declaration_new(const char *length,
			  struct declaration *elem_declaration,
			  struct declaration_scope *parent_scope)
{
	struct declaration_sequence *sequence_declaration;
	struct declaration *declaration;

	sequence_declaration = g_new(struct declaration_sequence, 1);
	declaration = &sequence_declaration->p;

	sequence_declaration->length_name = g_array_new(FALSE, TRUE, sizeof(GQuark));
	append_scope_path(length, sequence_declaration->length_name);

	declaration_ref(elem_declaration);
	sequence_declaration->elem = elem_declaration;
	sequence_declaration->scope = new_declaration_scope(parent_scope);
	declaration->id = CTF_TYPE_SEQUENCE;
	declaration->alignment = elem_declaration->alignment;
	declaration->declaration_free = _sequence_declaration_free;
	declaration->definition_new = _sequence_definition_new;
	declaration->definition_free = _sequence_definition_free;
	declaration->ref = 1;
	return sequence_declaration;
}

static
struct definition *_sequence_definition_new(struct declaration *declaration,
				struct definition_scope *parent_scope,
				GQuark field_name, int index,
				const char *root_name)
{
	struct declaration_sequence *sequence_declaration =
		container_of(declaration, struct declaration_sequence, p);
	struct definition_sequence *sequence;
	struct definition *len_parent;
	int ret;

	sequence = g_new(struct definition_sequence, 1);
	declaration_ref(&sequence_declaration->p);
	sequence->p.declaration = declaration;
	sequence->declaration = sequence_declaration;
	sequence->p.ref = 1;
	/*
	 * Use INT_MAX order to ensure that all fields of the parent
	 * scope are seen as being prior to this scope.
	 */
	sequence->p.index = root_name ? INT_MAX : index;
	sequence->p.name = field_name;
	sequence->p.path = new_definition_path(parent_scope, field_name, root_name);
	sequence->p.scope = new_definition_scope(parent_scope, field_name, root_name);
	ret = register_field_definition(field_name, &sequence->p,
					parent_scope);
	assert(!ret);
	len_parent = lookup_path_definition(sequence->p.scope->scope_path,
					    sequence_declaration->length_name,
					    parent_scope);
	if (!len_parent) {
		printf("[error] Lookup for sequence length field failed.\n");
		goto error;
	}
	sequence->length =
		container_of(len_parent, struct definition_integer, p);
	if (sequence->length->declaration->signedness) {
		printf("[error] Sequence length field should be unsigned.\n");
		goto error;
	}
	definition_ref(len_parent);

	sequence->string = NULL;
	sequence->elems = NULL;

	if (sequence_declaration->elem->id == CTF_TYPE_INTEGER) {
		struct declaration_integer *integer_declaration =
			container_of(sequence_declaration->elem, struct declaration_integer, p);

		if (integer_declaration->encoding == CTF_STRING_UTF8
		      || integer_declaration->encoding == CTF_STRING_ASCII) {

			sequence->string = g_string_new("");

			if (integer_declaration->len == CHAR_BIT
			    && integer_declaration->p.alignment == CHAR_BIT) {
				return &sequence->p;
			}
		}
	}

	sequence->elems = g_ptr_array_new();
	return &sequence->p;

error:
	free_definition_scope(sequence->p.scope);
	declaration_unref(&sequence_declaration->p);
	g_free(sequence);
	return NULL;
}

static
void _sequence_definition_free(struct definition *definition)
{
	struct definition_sequence *sequence =
		container_of(definition, struct definition_sequence, p);
	struct definition *len_definition = &sequence->length->p;
	uint64_t i;

	if (sequence->string)
		(void) g_string_free(sequence->string, TRUE);
	if (sequence->elems) {
		for (i = 0; i < sequence->elems->len; i++) {
			struct definition *field;

			field = g_ptr_array_index(sequence->elems, i);
			field->declaration->definition_free(field);
		}
	}
	(void) g_ptr_array_free(sequence->elems, TRUE);
	definition_unref(len_definition);
	free_definition_scope(sequence->p.scope);
	declaration_unref(sequence->p.declaration);
	g_free(sequence);
}

uint64_t sequence_len(struct definition_sequence *sequence)
{
	return sequence->length->value._unsigned;
}

struct definition *sequence_index(struct definition_sequence *sequence, uint64_t i)
{
	if (!sequence->elems)
		return NULL;
	if (i >= sequence->length->value._unsigned)
		return NULL;
	assert(i < sequence->elems->len);
	return g_ptr_array_index(sequence->elems, i);
}
