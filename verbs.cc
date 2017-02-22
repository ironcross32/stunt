/******************************************************************************
  Copyright (c) 1992, 1995, 1996 Xerox Corporation.  All rights reserved.
  Portions of this code were written by Stephen White, aka ghond.
  Use and copying of this software and preparation of derivative works based
  upon this software are permitted.  Any distribution of this software or
  derivative works must comply with all applicable United States export
  control laws.  This software is made available AS IS, and Xerox Corporation
  makes no warranty about the software, its performance or its conformity to
  any specification.  Any person obtaining a copy of this software is requested
  to send their name and post office or electronic mail address to:
    Pavel Curtis
    Xerox PARC
    3333 Coyote Hill Rd.
    Palo Alto, CA 94304
    Pavel@Xerox.Com
 *****************************************************************************/

#include <assert.h>

#include "my-string.h"

#include "config.h"
#include "db.h"
#include "execute.h"
#include "functions.h"
#include "list.h"
#include "log.h"
#include "match.h"
#include "parse_cmd.h"
#include "parser.h"
#include "server.h"
#include "storage.h"
#include "structures.h"
#include "str_intern.h"
#include "unparse.h"
#include "utils.h"
#include "verbs.h"

struct verb_data {
    int i;
    List l;
};

static int
add_to_list(void *data, const ref_ptr<const char>& verb_name)
{
    struct verb_data *d = (struct verb_data *)data;

    d->i++;
    d->l[d->i] = str_ref_to_var(verb_name);

    return 0;
}

static package
bf_verbs(const List& arglist, Objid progr)
{				/* (object) */
    Var obj = arglist[1];

    free_var(arglist);

    if (!obj.is_object())
	return make_error_pack(E_TYPE);
    else if (!is_valid(obj))
	return make_error_pack(E_INVARG);
    else if (!db_object_allows(obj, progr, FLAG_READ))
	return make_error_pack(E_PERM);
    else {
	struct verb_data d;
	d.i = 0;
	d.l = new_list(db_count_verbs(obj));
	db_for_all_verbs(obj, add_to_list, &d);

	return make_var_pack(d.l);
    }
}

static enum error
validate_verb_info(const List& v, Objid * owner, unsigned *flags, ref_ptr<const char>* names)
{
    if (!(v.length() == 3 && v[1].is_obj() && v[2].is_str() && v[3].is_str()))
	return E_TYPE;

    *owner = v[1].v.obj;
    if (!valid(*owner))
	return E_INVARG;

    *flags = 0;
    for (const char* s = v[2].v.str.expose(); *s; s++) {
	switch (*s) {
	case 'r':
	case 'R':
	    *flags |= VF_READ;
	    break;
	case 'w':
	case 'W':
	    *flags |= VF_WRITE;
	    break;
	case 'x':
	case 'X':
	    *flags |= VF_EXEC;
	    break;
	case 'd':
	case 'D':
	    *flags |= VF_DEBUG;
	    break;
	default:
	    return E_INVARG;
	}
    }

    const char* new_names = v[3].v.str.expose();
    while (*new_names == ' ')
	new_names++;
    if (*new_names == '\0')
	return E_INVARG;

    *names = str_dup(new_names);

    return E_NONE;
}

static int
match_arg_spec(const ref_ptr<const char>& s, db_arg_spec * spec)
{
    if (!mystrcasecmp(s.expose(), "none")) {
	*spec = ASPEC_NONE;
	return 1;
    } else if (!mystrcasecmp(s.expose(), "any")) {
	*spec = ASPEC_ANY;
	return 1;
    } else if (!mystrcasecmp(s.expose(), "this")) {
	*spec = ASPEC_THIS;
	return 1;
    } else
	return 0;
}

static int
match_prep_spec(const ref_ptr<const char>& s, db_prep_spec * spec)
{
    if (!mystrcasecmp(s.expose(), "none")) {
	*spec = PREP_NONE;
	return 1;
    } else if (!mystrcasecmp(s.expose(), "any")) {
	*spec = PREP_ANY;
	return 1;
    } else
	return (*spec = db_match_prep(s.expose())) != PREP_NONE;
}

static enum error
validate_verb_args(const List& v, db_arg_spec * dobj, db_prep_spec * prep,
		   db_arg_spec * iobj)
{
    if (!(v.length() == 3 && v[1].is_str() && v[2].is_str() && v[3].is_str()))
	return E_TYPE;

    if (!match_arg_spec(v[1].v.str, dobj)
	|| !match_prep_spec(v[2].v.str, prep)
	|| !match_arg_spec(v[3].v.str, iobj))
	return E_INVARG;

    return E_NONE;
}

static package
bf_add_verb(const List& arglist, Objid progr)
{				/* (object, info, args) */
    Var obj = arglist[1];
    assert(arglist[2].is_list());
    const List& info = static_cast<const List&>(arglist[2]);
    assert(arglist[3].is_list());
    const List& args = static_cast<const List&>(arglist[3]);
    Var result;
    Objid owner;
    unsigned flags;
    ref_ptr<const char> names;
    db_arg_spec dobj, iobj;
    db_prep_spec prep;
    enum error e;

    if ((e = validate_verb_info(info, &owner, &flags, &names)) != E_NONE)
	; /* already failed */
    else if ((e = validate_verb_args(args, &dobj, &prep, &iobj)) != E_NONE)
	free_str(names);
    else if (!obj.is_object()) {
	free_str(names);
	e = E_INVARG;
    } else if (!is_valid(obj)) {
	free_str(names);
	e = E_INVARG;
    } else if (!db_object_allows(obj, progr, FLAG_WRITE)
	       || (progr != owner && !is_wizard(progr))) {
	free_str(names);
	e = E_PERM;
    } else {
	result = Var::new_int(db_add_verb(obj, names, owner, flags, dobj, prep, iobj));
    }

    free_var(arglist);

    if (e == E_NONE)
	return make_var_pack(result);
    else
	return make_error_pack(e);
}

enum error
validate_verb_descriptor(const Var& desc)
{
    if (desc.is_str())
	return E_NONE;
    else if (!desc.is_int())
	return E_TYPE;
    else if (desc.v.num <= 0)
	return E_INVARG;
    else
	return E_NONE;
}

db_verb_handle
find_described_verb(const Var& obj, const Var& desc)
{
    if (desc.is_int())
	return db_find_indexed_verb(obj, desc.v.num);
    else {
	int flag = server_flag_option("support_numeric_verbname_strings", 0);
	return db_find_defined_verb(obj, desc.v.str, flag);
    }
}

static package
bf_delete_verb(const List& arglist, Objid progr)
{				/* (object, verb-desc) */
    Var obj = arglist[1];
    Var desc = arglist[2];
    db_verb_handle h;
    enum error e;

    if ((e = validate_verb_descriptor(desc)) != E_NONE)
	; /* e is already set */
    else if (!obj.is_object())
	e = E_TYPE;
    else if (!is_valid(obj))
	e = E_INVARG;
    else if (!db_object_allows(obj, progr, FLAG_WRITE))
	e = E_PERM;
    else {
	h = find_described_verb(obj, desc);
	if (h.ptr)
	    db_delete_verb(h);
	else
	    e = E_VERBNF;
    }

    free_var(arglist);

    if (e == E_NONE)
	return no_var_pack();
    else
	return make_error_pack(e);
}

static package
bf_verb_info(const List& arglist, Objid progr)
{				/* (object, verb-desc) */
    Var obj = arglist[1];
    Var desc = arglist[2];
    db_verb_handle h;
    List r;
    unsigned flags;
    char perms[5], *s;
    enum error e;

    if (!obj.is_object()) {
	free_var(arglist);
	return make_error_pack(E_TYPE);
    } else if ((e = validate_verb_descriptor(desc)) != E_NONE
	|| (e = E_INVARG, !is_valid(obj))) {
	free_var(arglist);
	return make_error_pack(e);
    }
    h = find_described_verb(obj, desc);
    free_var(arglist);

    if (!h.ptr)
	return make_error_pack(E_VERBNF);
    else if (!db_verb_allows(h, progr, VF_READ))
	return make_error_pack(E_PERM);

    s = perms;
    flags = db_verb_flags(h);
    if (flags & VF_READ)
	*s++ = 'r';
    if (flags & VF_WRITE)
	*s++ = 'w';
    if (flags & VF_EXEC)
	*s++ = 'x';
    if (flags & VF_DEBUG)
	*s++ = 'd';
    *s = '\0';
    r = new_list(3);
    r[1] = Var::new_obj(db_verb_owner(h));
    r[2] = Var::new_str(perms);
    r[3] = Var::new_str(db_verb_names(h));

    return make_var_pack(r);
}

static package
bf_set_verb_info(const List& arglist, Objid progr)
{				/* (object, verb-desc, {owner, flags, names}) */
    Var obj = arglist[1];
    Var desc = arglist[2];
    assert(arglist[3].is_list());
    const List& info = static_cast<const List&>(arglist[3]);
    Objid new_owner;
    unsigned new_flags;
    ref_ptr<const char> new_names;
    enum error e;
    db_verb_handle h;

    if ((e = validate_verb_descriptor(desc)) != E_NONE)
	; /* e is already set */
    else if (!obj.is_object())
	e = E_TYPE;
    else if (!is_valid(obj))
	e = E_INVARG;
    else
	e = validate_verb_info(info, &new_owner, &new_flags, &new_names);

    if (e != E_NONE) {
	free_var(arglist);
	return make_error_pack(e);
    }

    h = find_described_verb(obj, desc);

    free_var(arglist);

    if (!h.ptr) {
	free_str(new_names);
	return make_error_pack(E_VERBNF);
    } else if (!db_verb_allows(h, progr, VF_WRITE)
	       || (!is_wizard(progr) && db_verb_owner(h) != new_owner)) {
	free_str(new_names);
	return make_error_pack(E_PERM);
    }
    db_set_verb_owner(h, new_owner);
    db_set_verb_flags(h, new_flags);
    db_set_verb_names(h, new_names);

    return no_var_pack();
}

static ref_ptr<const char>
unparse_arg_spec(db_arg_spec spec)
{
    switch (spec) {
    case ASPEC_NONE:
	return str_intern("none");
    case ASPEC_ANY:
	return str_intern("any");
    case ASPEC_THIS:
	return str_intern("this");
    default:
	panic("UNPARSE_ARG_SPEC: Unknown db_arg_spec!");
    }
}

static package
bf_verb_args(const List& arglist, Objid progr)
{				/* (object, verb-desc) */
    Var obj = arglist[1];
    Var desc = arglist[2];
    db_verb_handle h;
    db_arg_spec dobj, iobj;
    db_prep_spec prep;
    List r;
    enum error e;

    if (!obj.is_object()) {
	free_var(arglist);
	return make_error_pack(E_TYPE);
    } if ((e = validate_verb_descriptor(desc)) != E_NONE
	|| (e = E_INVARG, !is_valid(obj))) {
	free_var(arglist);
	return make_error_pack(e);
    }
    h = find_described_verb(obj, desc);
    free_var(arglist);

    if (!h.ptr)
	return make_error_pack(E_VERBNF);
    else if (!db_verb_allows(h, progr, VF_READ))
	return make_error_pack(E_PERM);

    db_verb_arg_specs(h, &dobj, &prep, &iobj);
    r = new_list(3);
    r[1].type = TYPE_STR;
    r[1].v.str = unparse_arg_spec(dobj);
    r[2].type = TYPE_STR;
    r[2].v.str = str_dup(db_unparse_prep(prep));
    r[3].type = TYPE_STR;
    r[3].v.str = unparse_arg_spec(iobj);

    return make_var_pack(r);
}

static package
bf_set_verb_args(const List& arglist, Objid progr)
{				/* (object, verb-desc, {dobj, prep, iobj}) */
    Var obj = arglist[1];
    Var desc = arglist[2];
    assert(arglist[3].is_list());
    const List& args = static_cast<const List&>(arglist[3]);
    enum error e;
    db_verb_handle h;
    db_arg_spec dobj, iobj;
    db_prep_spec prep;

    if ((e = validate_verb_descriptor(desc)) != E_NONE)
	; /* e is already set */
    else if (!obj.is_object())
	e = E_TYPE;
    else if (!is_valid(obj))
	e = E_INVARG;
    else
	e = validate_verb_args(args, &dobj, &prep, &iobj);

    if (e != E_NONE) {
	free_var(arglist);
	return make_error_pack(e);
    }
    h = find_described_verb(obj, desc);
    free_var(arglist);

    if (!h.ptr)
	return make_error_pack(E_VERBNF);
    else if (!db_verb_allows(h, progr, VF_WRITE))
	return make_error_pack(E_PERM);

    db_set_verb_arg_specs(h, dobj, prep, iobj);

    return no_var_pack();
}

static void
lister(void* data, const char* line)
{
    List *r = (List *) data;
    Var v = Var::new_str(line);
    *r = listappend(*r, v);
}

static package
bf_verb_code(const List& arglist, Objid progr)
{				/* (object, verb-desc [, fully-paren [, indent]]) */
    int nargs = arglist.length();
    Var obj = arglist[1];
    Var desc = arglist[2];
    int parens = nargs >= 3 && is_true(arglist[3]);
    int indent = nargs < 4 || is_true(arglist[4]);
    db_verb_handle h;
    Var code;
    enum error e;

    if (!obj.is_object()) {
	free_var(arglist);
	return make_error_pack(E_TYPE);
    } else if ((e = validate_verb_descriptor(desc)) != E_NONE
	|| (e = E_INVARG, !is_valid(obj))) {
	free_var(arglist);
	return make_error_pack(e);
    }
    h = find_described_verb(obj, desc);
    free_var(arglist);

    if (!h.ptr)
	return make_error_pack(E_VERBNF);
    else if (!db_verb_allows(h, progr, VF_READ))
	return make_error_pack(E_PERM);

    code = new_list(0);
    unparse_program(db_verb_program(h), lister, &code, parens, indent,
		    MAIN_VECTOR);

    return make_var_pack(code);
}

static package
bf_set_verb_code(const List& arglist, Objid progr)
{				/* (object, verb-desc, code) */
    assert(arglist[3].is_list());

    Var obj = arglist[1];
    Var desc = arglist[2];
    const List& code = static_cast<const List&>(arglist[3]);
    int i;
    Program *program;
    db_verb_handle h;
    List errors;
    enum error e;

    for (i = 1; i <= code.length(); i++)
	if (!code[i].is_str()) {
	    free_var(arglist);
	    return make_error_pack(E_TYPE);
	}
    if (!obj.is_object()) {
	free_var(arglist);
	return make_error_pack(E_TYPE);
    } else if ((e = validate_verb_descriptor(desc)) != E_NONE
	|| (e = E_INVARG, !is_valid(obj))) {
	free_var(arglist);
	return make_error_pack(e);
    }
    h = find_described_verb(obj, desc);
    if (!h.ptr) {
	free_var(arglist);
	return make_error_pack(E_VERBNF);
    } else if (!is_programmer(progr) || !db_verb_allows(h, progr, VF_WRITE)) {
	free_var(arglist);
	return make_error_pack(E_PERM);
    }
    program = parse_list_as_program(code, &errors);
    if (program) {
	if (task_timed_out)
	    free_program(program);
	else
	    db_set_verb_program(h, program);
    }
    free_var(arglist);
    return make_var_pack(errors);
}

static package
bf_respond_to(const List& arglist, Objid progr)
{
    Var object = arglist[1];
    const ref_ptr<const char>& verb = arglist[2].v.str;

    if (!object.is_object()) {
	free_var(arglist);
	return make_error_pack(E_TYPE);
    } else if (!is_valid(object)) {
	free_var(arglist);
	return make_error_pack(E_INVARG);
    }

    db_verb_handle h = db_find_callable_verb(object, verb);

    free_var(arglist);

    Var r;

    if (h.ptr) {
	if (db_object_allows(object, progr, FLAG_READ)) {
	    List l;
	    r = l = new_list(2);
	    l[1] = var_ref(db_verb_definer(h));
	    l[2] = str_ref_to_var(db_verb_names(h));
	}
	else {
	    r = Var::new_int(1);
	}
    }
    else {
	r = Var::new_int(0);
    }

    return make_var_pack(r);
}

static int
all_strings(const Var& arglist)
{
    Var arg;
    int i, c;

    FOR_EACH (arg, arglist, i, c)
	if (!arg.is_str())
	    return 0;

    return 1;
}

static package
bf_eval(const Var& value, Objid progr, Byte next, bf_call_data* vdata)
{
    package p;
    if (next == 1) {

	if (!is_programmer(progr)) {
	    free_var(value);
	    p = make_error_pack(E_PERM);
	} else if (!all_strings(value)) {
	    free_var(value);
	    p = make_error_pack(E_TYPE);
	} else {
	    const List& code = static_cast<const List&>(value);
	    List errors;
	    Program *program = parse_list_as_program(code, &errors);

	    free_var(value);

	    if (program) {
		free_var(errors);
		if (setup_activ_for_eval(program))
		    p = make_call_pack(2, 0);
		else {
		    free_program(program);
		    p = make_error_pack(E_MAXREC);
		}
	    } else {
		List r = new_list(2);
		r[1] = Var::new_int(0);
		r[2] = errors;
		p = make_var_pack(r);
	    }
	}
    } else {			/* next == 2 */
	List r = new_list(2);
	r[1] = Var::new_int(1);
	r[2] = value;
	p = make_var_pack(r);
    }
    return p;
}

void
register_verbs(void)
{
    register_function("verbs", 1, 1, bf_verbs,
		      TYPE_ANY);
    register_function("verb_info", 2, 2, bf_verb_info,
		      TYPE_ANY, TYPE_ANY);
    register_function("set_verb_info", 3, 3, bf_set_verb_info,
		      TYPE_ANY, TYPE_ANY, TYPE_LIST);
    register_function("verb_args", 2, 2, bf_verb_args,
		      TYPE_ANY, TYPE_ANY);
    register_function("set_verb_args", 3, 3, bf_set_verb_args,
		      TYPE_ANY, TYPE_ANY, TYPE_LIST);
    register_function("add_verb", 3, 3, bf_add_verb,
		      TYPE_ANY, TYPE_LIST, TYPE_LIST);
    register_function("delete_verb", 2, 2, bf_delete_verb,
		      TYPE_ANY, TYPE_ANY);
    register_function("verb_code", 2, 4, bf_verb_code,
		      TYPE_ANY, TYPE_ANY, TYPE_ANY, TYPE_ANY);
    register_function("set_verb_code", 3, 3, bf_set_verb_code,
		      TYPE_ANY, TYPE_ANY, TYPE_LIST);
    register_function("respond_to", 2, 2, bf_respond_to,
		      TYPE_ANY, TYPE_STR);
    register_function("eval", 1, -1, bf_eval, TYPE_STR);
}
