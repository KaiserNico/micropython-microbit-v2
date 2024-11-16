//Nico Kaiser
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>

#include "py/mphal.h"
#include "py/obj.h"
#include "py/objarray.h"
#include "py/objtuple.h"
#include "py/objtype.h"
#include "py/runtime.h"

#include "modneopixel.h"

#if MICROPY_DEBUG_VERBOSE // print debugging info
#define DEBUG_PRINT (1)
#define DEBUG_printf DEBUG_printf
#else // don't print debugging info
#define DEBUG_PRINT (0)
#define DEBUG_printf(...) (void)0
#endif

#define ENABLE_SPECIAL_ACCESSORS \
    (MICROPY_PY_DESCRIPTORS || MICROPY_PY_DELATTR_SETATTR || MICROPY_PY_BUILTINS_PROPERTY)

#define COLOR_INDEX_RED (1)
#define COLOR_INDEX_GREEN (0)
#define COLOR_INDEX_BLUE (2)
#define COLOR_INDEX_WHITE (3)
#define COLOR_INDEX_MAP (COLOR_INDEX_WHITE << 12 | COLOR_INDEX_BLUE << 8 | COLOR_INDEX_GREEN << 4 | COLOR_INDEX_RED)

//assert function for neopixel
void assert_neopixel(mp_obj_t self_in) {
    if (mp_obj_get_type(self_in) != &mod_NeoPixel_type) {
        mp_raise_TypeError(MP_ERROR_TEXT("Expected a neopixel object"));
    }
}

//copied and modified from py/objtype.c
struct class_lookup_data {
    mp_obj_instance_t *obj;
    qstr attr;
    size_t meth_offset;
    mp_obj_t *dest;
    bool is_type;
};

STATIC void mp_obj_class_lookup(struct class_lookup_data *lookup, const mp_obj_type_t *type) {
    assert(lookup->dest[0] == MP_OBJ_NULL);
    assert(lookup->dest[1] == MP_OBJ_NULL);
    for (;;) {
        DEBUG_printf("mp_obj_class_lookup: Looking up %s in %s\n", qstr_str(lookup->attr), qstr_str(type->name));
        // Optimize special method lookup for native types
        // This avoids extra method_name => slot lookup. On the other hand,
        // this should not be applied to class types, as will result in extra
        // lookup either.
        if (type->locals_dict != NULL) {
            // search locals_dict (the set of methods/attributes)
            assert(mp_obj_is_dict_or_ordereddict(MP_OBJ_FROM_PTR(type->locals_dict))); // MicroPython restriction, for now
            mp_map_t *locals_map = &type->locals_dict->map;
            mp_map_elem_t *elem = mp_map_lookup(locals_map, MP_OBJ_NEW_QSTR(lookup->attr), MP_MAP_LOOKUP);
            if (elem != NULL) {
                if (lookup->is_type) {
                    // If we look up a class method, we need to return original type for which we
                    // do a lookup, not a (base) type in which we found the class method.
                    const mp_obj_type_t *org_type = (const mp_obj_type_t *)lookup->obj;
                    mp_convert_member_lookup(MP_OBJ_NULL, org_type, elem->value, lookup->dest);
                } else {
                    mp_obj_instance_t *obj = lookup->obj;
                    mp_obj_t obj_obj;
                    obj_obj = MP_OBJ_FROM_PTR(obj);
                    mp_convert_member_lookup(obj_obj, type, elem->value, lookup->dest);
                }
                #if DEBUG_PRINT
                DEBUG_printf("mp_obj_class_lookup: Returning: ");
                mp_obj_print_helper(MICROPY_DEBUG_PRINTER, lookup->dest[0], PRINT_REPR);
                if (lookup->dest[1] != MP_OBJ_NULL) {
                    // Don't try to repr() lookup->dest[1], as we can be called recursively
                    DEBUG_printf(" <%s @%p>", mp_obj_get_type_str(lookup->dest[1]), MP_OBJ_TO_PTR(lookup->dest[1]));
                }
                DEBUG_printf("\n");
                #endif
                return;
            }
        }

        // Previous code block takes care about attributes defined in .locals_dict,
        // but some attributes of native types may be handled using .load_attr method,
        // so make sure we try to lookup those too.
        if (lookup->obj != NULL && !lookup->is_type && mp_obj_is_native_type(type) && type != &mp_type_object /* object is not a real type */) {
            mp_load_method_maybe(lookup->obj->subobj[0], lookup->attr, lookup->dest);
            if (lookup->dest[0] != MP_OBJ_NULL) {
                return;
            }
        }

        // attribute not found, keep searching base classes

        if (type->parent == NULL) {
            DEBUG_printf("mp_obj_class_lookup: No more parents\n");
            return;
        #if MICROPY_MULTIPLE_INHERITANCE
        } else if (((mp_obj_base_t *)type->parent)->type == &mp_type_tuple) {
            const mp_obj_tuple_t *parent_tuple = type->parent;
            const mp_obj_t *item = parent_tuple->items;
            const mp_obj_t *top = item + parent_tuple->len - 1;
            for (; item < top; ++item) {
                assert(mp_obj_is_type(*item, &mp_type_type));
                mp_obj_type_t *bt = (mp_obj_type_t *)MP_OBJ_TO_PTR(*item);
                if (bt == &mp_type_object) {
                    // Not a "real" type
                    continue;
                }
                mp_obj_class_lookup(lookup, bt);
                if (lookup->dest[0] != MP_OBJ_NULL) {
                    return;
                }
            }

            // search last base (simple tail recursion elimination)
            assert(mp_obj_is_type(*item, &mp_type_type));
            type = (mp_obj_type_t *)MP_OBJ_TO_PTR(*item);
        #endif
        } else {
            type = type->parent;
        }
        if (type == &mp_type_object) {
            // Not a "real" type
            return;
        }
    }
}

STATIC void mp_obj_NeoPixel_load_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    // logic: look in instance members then class locals
    assert_neopixel(self_in);
    mp_obj_instance_t *self = MP_OBJ_TO_PTR(self_in);

    // Note: This is fast-path'ed in the VM for the MP_BC_LOAD_ATTR operation.
    mp_map_elem_t *elem = mp_map_lookup(&self->members, MP_OBJ_NEW_QSTR(attr), MP_MAP_LOOKUP);
    if (elem != NULL) {
        // object member, always treated as a value
        dest[0] = elem->value;
        return;
    }
    #if MICROPY_CPYTHON_COMPAT
    if (attr == MP_QSTR___dict__) {
        // Create a new dict with a copy of the instance's map items.
        // This creates, unlike CPython, a read-only __dict__ that can't be modified.
        mp_obj_dict_t dict;
        dict.base.type = &mp_type_dict;
        dict.map = self->members;
        dest[0] = mp_obj_dict_copy(MP_OBJ_FROM_PTR(&dict));
        mp_obj_dict_t *dest_dict = MP_OBJ_TO_PTR(dest[0]);
        dest_dict->map.is_fixed = 1;
        return;
    }
    #endif
    struct class_lookup_data lookup = {
        .obj = self,
        .attr = attr,
        .meth_offset = 0,
        .dest = dest,
        .is_type = false,
    };
    mp_obj_class_lookup(&lookup, self->base.type);
    mp_obj_t member = dest[0];
    if (member != MP_OBJ_NULL) {
        if (!(self->base.type->flags & MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS)) {
            // Class doesn't have any special accessors to check so return straightaway
            return;
        }

        #if MICROPY_PY_BUILTINS_PROPERTY
        if (mp_obj_is_type(member, &mp_type_property)) {
            // object member is a property; delegate the load to the property
            // Note: This is an optimisation for code size and execution time.
            // The proper way to do it is have the functionality just below
            // in a __get__ method of the property object, and then it would
            // be called by the descriptor code down below.  But that way
            // requires overhead for the nested mp_call's and overhead for
            // the code.
            const mp_obj_t *proxy = mp_obj_property_get(member);
            if (proxy[0] == mp_const_none) {
                mp_raise_msg(&mp_type_AttributeError, MP_ERROR_TEXT("unreadable attribute"));
            } else {
                dest[0] = mp_call_function_n_kw(proxy[0], 1, 0, &self_in);
            }
            return;
        }
        #endif

        #if MICROPY_PY_DESCRIPTORS
        // found a class attribute; if it has a __get__ method then call it with the
        // class instance and class as arguments and return the result
        // Note that this is functionally correct but very slow: each load_attr
        // requires an extra mp_load_method_maybe to check for the __get__.
        mp_obj_t attr_get_method[4];
        mp_load_method_maybe(member, MP_QSTR___get__, attr_get_method);
        if (attr_get_method[0] != MP_OBJ_NULL) {
            attr_get_method[2] = self_in;
            attr_get_method[3] = MP_OBJ_FROM_PTR(mp_obj_get_type(self_in));
            dest[0] = mp_call_method_n_kw(2, 0, attr_get_method);
        }
        #endif
        return;
    }

    // try __getattr__
    if (attr != MP_QSTR___getattr__) {
        #if MICROPY_PY_DELATTR_SETATTR
        // If the requested attr is __setattr__/__delattr__ then don't delegate the lookup
        // to __getattr__.  If we followed CPython's behaviour then __setattr__/__delattr__
        // would have already been found in the "object" base class.
        if (attr == MP_QSTR___setattr__ || attr == MP_QSTR___delattr__) {
            return;
        }
        #endif

        mp_obj_t dest2[3];
        mp_load_method_maybe(self_in, MP_QSTR___getattr__, dest2);
        if (dest2[0] != MP_OBJ_NULL) {
            // __getattr__ exists, call it and return its result
            dest2[2] = MP_OBJ_NEW_QSTR(attr);
            dest[0] = mp_call_method_n_kw(1, 0, dest2);
            return;
        }
    }
}

STATIC bool mp_obj_NeoPixel_store_attr(mp_obj_t self_in, qstr attr, mp_obj_t value) {
	assert_neopixel(self_in);
    mp_obj_instance_t *self = MP_OBJ_TO_PTR(self_in);

    if (!(self->base.type->flags & MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS)) {
        // Class doesn't have any special accessors so skip their checks
        goto skip_special_accessors;
    }

    #if MICROPY_PY_BUILTINS_PROPERTY || MICROPY_PY_DESCRIPTORS
    // With property and/or descriptors enabled we need to do a lookup
    // first in the class dict for the attribute to see if the store should
    // be delegated.
    mp_obj_t member[2] = {MP_OBJ_NULL};
    struct class_lookup_data lookup = {
        .obj = self,
        .attr = attr,
        .meth_offset = 0,
        .dest = member,
        .is_type = false,
    };
    mp_obj_class_lookup(&lookup, self->base.type);

    if (member[0] != MP_OBJ_NULL) {
        #if MICROPY_PY_BUILTINS_PROPERTY
        if (mp_obj_is_type(member[0], &mp_type_property)) {
            // attribute exists and is a property; delegate the store/delete
            // Note: This is an optimisation for code size and execution time.
            // The proper way to do it is have the functionality just below in
            // a __set__/__delete__ method of the property object, and then it
            // would be called by the descriptor code down below.  But that way
            // requires overhead for the nested mp_call's and overhead for
            // the code.
            const mp_obj_t *proxy = mp_obj_property_get(member[0]);
            mp_obj_t dest[2] = {self_in, value};
            if (value == MP_OBJ_NULL) {
                // delete attribute
                if (proxy[2] == mp_const_none) {
                    // TODO better error message?
                    return false;
                } else {
                    mp_call_function_n_kw(proxy[2], 1, 0, dest);
                    return true;
                }
            } else {
                // store attribute
                if (proxy[1] == mp_const_none) {
                    // TODO better error message?
                    return false;
                } else {
                    mp_call_function_n_kw(proxy[1], 2, 0, dest);
                    return true;
                }
            }
        }
        #endif

        #if MICROPY_PY_DESCRIPTORS
        // found a class attribute; if it has a __set__/__delete__ method then
        // call it with the class instance (and value) as arguments
        if (value == MP_OBJ_NULL) {
            // delete attribute
            mp_obj_t attr_delete_method[3];
            mp_load_method_maybe(member[0], MP_QSTR___delete__, attr_delete_method);
            if (attr_delete_method[0] != MP_OBJ_NULL) {
                attr_delete_method[2] = self_in;
                mp_call_method_n_kw(1, 0, attr_delete_method);
                return true;
            }
        } else {
            // store attribute
            mp_obj_t attr_set_method[4];
            mp_load_method_maybe(member[0], MP_QSTR___set__, attr_set_method);
            if (attr_set_method[0] != MP_OBJ_NULL) {
                attr_set_method[2] = self_in;
                attr_set_method[3] = value;
                mp_call_method_n_kw(2, 0, attr_set_method);
                return true;
            }
        }
        #endif
    }
    #endif

    #if MICROPY_PY_DELATTR_SETATTR
    if (value == MP_OBJ_NULL) {
        // delete attribute
        // try __delattr__ first
        mp_obj_t attr_delattr_method[3];
        mp_load_method_maybe(self_in, MP_QSTR___delattr__, attr_delattr_method);
        if (attr_delattr_method[0] != MP_OBJ_NULL) {
            // __delattr__ exists, so call it
            attr_delattr_method[2] = MP_OBJ_NEW_QSTR(attr);
            mp_call_method_n_kw(1, 0, attr_delattr_method);
            return true;
        }
    } else {
        // store attribute
        // try __setattr__ first
        mp_obj_t attr_setattr_method[4];
        mp_load_method_maybe(self_in, MP_QSTR___setattr__, attr_setattr_method);
        if (attr_setattr_method[0] != MP_OBJ_NULL) {
            // __setattr__ exists, so call it
            attr_setattr_method[2] = MP_OBJ_NEW_QSTR(attr);
            attr_setattr_method[3] = value;
            mp_call_method_n_kw(2, 0, attr_setattr_method);
            return true;
        }
    }
    #endif

skip_special_accessors:

    if (value == MP_OBJ_NULL) {
        // delete attribute
        mp_map_elem_t *elem = mp_map_lookup(&self->members, MP_OBJ_NEW_QSTR(attr), MP_MAP_LOOKUP_REMOVE_IF_FOUND);
        return elem != NULL;
    } else {
        // store attribute
        mp_map_lookup(&self->members, MP_OBJ_NEW_QSTR(attr), MP_MAP_LOOKUP_ADD_IF_NOT_FOUND)->value = value;
        return true;
    }
}

STATIC void mp_obj_NeoPixel_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] == MP_OBJ_NULL) {
        mp_obj_NeoPixel_load_attr(self_in, attr, dest);
    } else {
        if (mp_obj_NeoPixel_store_attr(self_in, attr, dest[1])) {
            dest[0] = MP_OBJ_NULL; // indicate success
        }
    }
}

mp_obj_t mp_obj_NeoPixel_make_new(const mp_obj_type_t *self, size_t n_args, size_t n_kw, const mp_obj_t *args) {

    // look for __new__ function
    mp_obj_t init_fn[2] = {MP_OBJ_NULL};
    struct class_lookup_data lookup = {
        .obj = NULL,
        .attr = MP_QSTR___new__,
        .meth_offset = offsetof(mp_obj_type_t, make_new),
        .dest = init_fn,
        .is_type = false,
    };
    mp_obj_class_lookup(&lookup, self);

    const mp_obj_type_t *native_base = NULL;
    mp_obj_instance_t *o;
    // create a blank instance.
    o = mp_obj_new_instance(self, &native_base);
    // now call Python class __init__ function with all args
    init_fn[0] = init_fn[1] = MP_OBJ_NULL;
    lookup.obj = o;
    lookup.attr = MP_QSTR___init__;
    lookup.meth_offset = 0;
    mp_obj_class_lookup(&lookup, self);
    if (init_fn[0] != MP_OBJ_NULL) {
        mp_obj_t init_ret;
        if (n_args == 0 && n_kw == 0) {
         	//Not used
            init_ret = mp_call_method_n_kw(0, 0, init_fn);
        } else {
            mp_obj_t *args2 = m_new(mp_obj_t, 2 + n_args + 2 * n_kw);
            args2[0] = init_fn[0];
            args2[1] = init_fn[1];
            memcpy(args2 + 2, args, (n_args + 2 * n_kw) * sizeof(mp_obj_t));
            init_ret = mp_call_method_n_kw(n_args, n_kw, args2);
            m_del(mp_obj_t, args2, 2 + n_args + 2 * n_kw);
        }
        if (init_ret != mp_const_none) {
            #if MICROPY_ERROR_REPORTING <= MICROPY_ERROR_REPORTING_TERSE
            mp_raise_TypeError(MP_ERROR_TEXT("__init__() should return None"));
            #else
            mp_raise_msg_varg(&mp_type_TypeError,
                MP_ERROR_TEXT("__init__() should return None, not '%s'"), mp_obj_get_type_str(init_ret));
            #endif
        }
    }

    return MP_OBJ_FROM_PTR(o);
}

STATIC void NeoPixel_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
	assert_neopixel(self_in);
	mp_obj_instance_t *self = MP_OBJ_TO_PTR(self_in);
	mp_printf(print, "<%s object at %p>", mp_obj_get_type_str(self_in), self);
}

STATIC mp_obj_t NeoPixel_subscr(mp_obj_t self_in, mp_obj_t index, mp_obj_t value) {
	assert_neopixel(self_in);
    mp_obj_instance_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t member[4] = {MP_OBJ_NULL, MP_OBJ_NULL, index, value};
    struct class_lookup_data lookup = {
        .obj = self,
        .meth_offset = offsetof(mp_obj_type_t, subscr),
        .dest = member,
        .is_type = false,
    };
    if (value == MP_OBJ_NULL) {
        // delete item
        lookup.attr = MP_QSTR___delitem__;
    } else if (value == MP_OBJ_SENTINEL) {
        // load item
        lookup.attr = MP_QSTR___getitem__;
    } else {
        // store item
        lookup.attr = MP_QSTR___setitem__;
    }
    mp_obj_class_lookup(&lookup, self->base.type);
    if (member[0] == MP_OBJ_SENTINEL) {
        return mp_obj_subscr(self->subobj[0], index, value);
    } else if (member[0] != MP_OBJ_NULL) {
        size_t n_args = value == MP_OBJ_NULL || value == MP_OBJ_SENTINEL ? 1 : 2;
        mp_obj_t ret = mp_call_method_n_kw(n_args, 0, member);
        if (value == MP_OBJ_SENTINEL) {
            return ret;
        } else {
            return mp_const_none;
        }
    } else {
        return MP_OBJ_NULL; // op not supported
    }
}

STATIC mp_obj_t NeoPixel_unary_op(mp_unary_op_t op, mp_obj_t self_in) {
	assert_neopixel(self_in);
	mp_obj_instance_t *self = MP_OBJ_TO_PTR(self_in);
    qstr op_name = mp_unary_op_method_name[op];
    /* Still try to lookup native slot
    if (op_name == 0) {
        return MP_OBJ_NULL;
    }
    */
    mp_obj_t member[2] = {MP_OBJ_NULL};
    struct class_lookup_data lookup = {
        .obj = self,
        .attr = op_name,
        .meth_offset = offsetof(mp_obj_type_t, unary_op),
        .dest = member,
        .is_type = false,
    };
    mp_obj_class_lookup(&lookup, self->base.type);
    if (member[0] == MP_OBJ_SENTINEL) {
        return mp_unary_op(op, self->subobj[0]);
    } else if (member[0] != MP_OBJ_NULL) {
        mp_obj_t val = mp_call_function_1(member[0], self_in);

        switch (op) {
            case MP_UNARY_OP_HASH:
                // __hash__ must return a small int
                val = MP_OBJ_NEW_SMALL_INT(mp_obj_get_int_truncated(val));
                break;
            case MP_UNARY_OP_INT:
                // Must return int
                if (!mp_obj_is_int(val)) {
                    mp_raise_TypeError(NULL);
                }
                break;
            default:
                // No need to do anything
                ;
        }
        return val;
    } else {
        if (op == MP_UNARY_OP_HASH) {
            lookup.attr = MP_QSTR___eq__;
            mp_obj_class_lookup(&lookup, self->base.type);
            if (member[0] == MP_OBJ_NULL) {
                // https://docs.python.org/3/reference/datamodel.html#object.__hash__
                // "User-defined classes have __eq__() and __hash__() methods by default;
                // with them, all objects compare unequal (except with themselves) and
                // x.__hash__() returns an appropriate value such that x == y implies
                // both that x is y and hash(x) == hash(y)."
                return MP_OBJ_NEW_SMALL_INT((mp_uint_t)self_in);
            }
            // "A class that overrides __eq__() and does not define __hash__() will have its __hash__() implicitly set to None.
            // When the __hash__() method of a class is None, instances of the class will raise an appropriate TypeError"
        }

        return MP_OBJ_NULL; // op not supported
    }
}

//End of copied section

STATIC const mp_obj_tuple_t mod_neopixel_ORDER_obj = {
    {&mp_type_tuple},
    4,
    {
      	MP_OBJ_NEW_SMALL_INT(COLOR_INDEX_RED),
        MP_OBJ_NEW_SMALL_INT(COLOR_INDEX_GREEN),
        MP_OBJ_NEW_SMALL_INT(COLOR_INDEX_BLUE),
        MP_OBJ_NEW_SMALL_INT(COLOR_INDEX_WHITE)
    }
};

STATIC mp_obj_t mod_neopixel___init___func(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
	mp_obj_instance_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    const mp_obj_t *sub_args = &pos_args[1];
    size_t sub_n_args = n_args - 1;

	enum { ARG_pin, ARG_n, ARG_bpp };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_pin, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_n, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_bpp, MP_ARG_INT, {.u_int = 3} },
    };
    mp_arg_val_t vals[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(sub_n_args, sub_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, vals);

    mp_int_t num_pixels = vals[ARG_n].u_int;
    mp_int_t bytes_per_pixel = vals[ARG_bpp].u_int;

    if (num_pixels <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid number of pixels"));;
    }

    if (!(bytes_per_pixel == 3 || bytes_per_pixel == 4)) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid bpp"));
    }
	self->base.type = &mod_NeoPixel_type;
	mp_store_attr(self,MP_QSTR_pin,vals[ARG_pin].u_obj);
	mp_store_attr(self,MP_QSTR_n,MP_OBJ_NEW_SMALL_INT(vals[ARG_n].u_int));
	mp_store_attr(self,MP_QSTR_bpp,MP_OBJ_NEW_SMALL_INT(vals[ARG_bpp].u_int));

    size_t buf_size = vals[ARG_bpp].u_int * vals[ARG_n].u_int;
    mp_obj_t bytearray_obj = mp_obj_new_bytearray(buf_size, NULL);
	mp_obj_array_t *bytearray_data = MP_OBJ_TO_PTR(bytearray_obj);
	size_t len = bytearray_data->len;
	memset(bytearray_data->items, 0, len);
	mp_store_attr(self,MP_QSTR_buf,bytearray_obj);

    mp_buffer_info_t buf_info;
    mp_get_buffer_raise(bytearray_obj, &buf_info, MP_BUFFER_READ);
    microbit_pin_obj_t* pin = (microbit_pin_obj_t*)microbit_obj_get_pin(vals[ARG_pin].u_obj);
    uintptr_t id = (uintptr_t)self;
    uint8_t pin_hal = pin->name;
    uint8_t n_hal = (uint8_t)mp_obj_get_int(MP_OBJ_NEW_SMALL_INT(vals[ARG_n].u_int));
    uint8_t bpp_hal = (uint8_t)mp_obj_get_int(MP_OBJ_NEW_SMALL_INT(vals[ARG_bpp].u_int));
    neopixel_hal_NeoPixel(id,pin_hal,n_hal,bpp_hal,buf_info.buf);

    return mp_const_none;
}

MP_DEFINE_CONST_FUN_OBJ_KW(mod_neopixel___init___obj,2, mod_neopixel___init___func);

STATIC mp_obj_t mod_neopixel___len___func(mp_obj_t self_in) {
    mp_obj_instance_t *self = MP_OBJ_TO_PTR(self_in);
    mp_map_elem_t *elem = mp_map_lookup(&self->members, MP_OBJ_NEW_QSTR(MP_QSTR_n), MP_MAP_LOOKUP);
    mp_obj_t n_value = elem->value;
    return MP_OBJ_NEW_SMALL_INT(mp_obj_get_int(n_value));
}

MP_DEFINE_CONST_FUN_OBJ_1(mod_neopixel___len___obj, mod_neopixel___len___func);

mp_obj_t mod_neopixel___set_item___func(mp_obj_t self_in, mp_obj_t index_in, mp_obj_t value) {
    mp_obj_instance_t *self = MP_OBJ_TO_PTR(self_in);
    size_t index = mp_obj_get_int(index_in);
    mp_obj_t *rgb;
    size_t len;
    mp_obj_tuple_get(value, &len, &rgb);
    index = index*len;
    mp_map_elem_t *elem = mp_map_lookup(&self->members, MP_OBJ_NEW_QSTR(MP_QSTR_buf), MP_MAP_LOOKUP);
    mp_obj_array_t *pixel_array = MP_OBJ_TO_PTR(elem->value);
    for (uint8_t i = 0; i < len; ++i) {
        mp_int_t color = mp_obj_get_int(rgb[i]);
        if (color > 255) {
        	color = color % 255;
        }
        if (color < 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid color"));
        }
        size_t index_now = index + ((COLOR_INDEX_MAP >> (4 * i)) & 0xf);
        ((uint8_t *)pixel_array->items)[index_now] = color;
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_3(mod_neopixel___set_item___obj, mod_neopixel___set_item___func);

mp_obj_t mod_neopixel___get_item___func(mp_obj_t self_in, mp_obj_t index_in) {
    mp_obj_instance_t *self = MP_OBJ_TO_PTR(self_in);
    size_t index = mp_obj_get_int(index_in);
    mp_map_elem_t *bpp_attr = mp_map_lookup(&self->members, MP_OBJ_NEW_QSTR(MP_QSTR_bpp), MP_MAP_LOOKUP);
    size_t bpp = (size_t)mp_obj_get_int(bpp_attr->value);
    size_t index_new = bpp * index;

    mp_map_elem_t *elem = mp_map_lookup(&self->members, MP_OBJ_NEW_QSTR(MP_QSTR_buf), MP_MAP_LOOKUP);
    mp_obj_array_t *pixel_array = MP_OBJ_TO_PTR(elem->value);
    mp_obj_t rgb[] = {
            MP_OBJ_NEW_SMALL_INT(((uint8_t*)pixel_array->items)[index_new + COLOR_INDEX_RED]),
            MP_OBJ_NEW_SMALL_INT(((uint8_t*)pixel_array->items)[index_new + COLOR_INDEX_GREEN]),
            MP_OBJ_NEW_SMALL_INT(((uint8_t*)pixel_array->items)[index_new + COLOR_INDEX_BLUE]),
            MP_OBJ_NEW_SMALL_INT(((uint8_t*)pixel_array->items)[index_new + COLOR_INDEX_WHITE]),
        };
    return mp_obj_new_tuple(bpp, rgb);;
}
MP_DEFINE_CONST_FUN_OBJ_2(mod_neopixel___get_item___obj, mod_neopixel___get_item___func);

STATIC mp_obj_t mod_neopixel_fill_func(mp_obj_t self_in, mp_obj_t color_tuple) {
    mp_obj_instance_t *self = MP_OBJ_TO_PTR(self_in);
    mp_map_elem_t *n_attr = mp_map_lookup(&self->members, MP_OBJ_NEW_QSTR(MP_QSTR_n), MP_MAP_LOOKUP);
    size_t index = (size_t)mp_obj_get_int(n_attr->value);

    for (size_t i = 0; i < index; ++i) {
        mp_obj_t index_in = mp_obj_new_int(i);
        NeoPixel_subscr(self_in, index_in, color_tuple);
    }

    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(mod_neopixel_fill_obj, mod_neopixel_fill_func);

STATIC mp_obj_t microbit_ws2812_write(mp_obj_t pin_in, mp_obj_t buf_in) {
    uint8_t pin = microbit_obj_get_pin(pin_in)->name;
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf_in, &bufinfo, MP_BUFFER_READ);
    microbit_hal_pin_write_ws2812(pin, bufinfo.buf, bufinfo.len);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(microbit_ws2812_write_obj, microbit_ws2812_write);

STATIC mp_obj_t mod_neopixel_write_func(mp_obj_t self_in) {
    mp_obj_instance_t *self = MP_OBJ_TO_PTR(self_in);

    mp_map_elem_t *pin_attr = mp_map_lookup(&self->members, MP_OBJ_NEW_QSTR(MP_QSTR_pin), MP_MAP_LOOKUP);
    mp_obj_t pin_value = pin_attr->value;

    mp_map_elem_t *buf_attr = mp_map_lookup(&self->members, MP_OBJ_NEW_QSTR(MP_QSTR_buf), MP_MAP_LOOKUP);
    mp_obj_t buf_value = buf_attr->value;

    microbit_ws2812_write(pin_value,buf_value);

    uintptr_t id = (uintptr_t)self;
    mp_map_elem_t *n_attr = mp_map_lookup(&self->members, MP_OBJ_NEW_QSTR(MP_QSTR_n), MP_MAP_LOOKUP);
    mp_obj_t n_value = n_attr->value;
    uint8_t n_hal = (uint8_t)mp_obj_get_int(n_value);
    mp_map_elem_t *bpp_attr = mp_map_lookup(&self->members, MP_OBJ_NEW_QSTR(MP_QSTR_bpp), MP_MAP_LOOKUP);
    mp_obj_t bpp_value = bpp_attr->value;
    uint8_t bpp_hal = (uint8_t)mp_obj_get_int(bpp_value);
    int length = n_hal * bpp_hal;
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf_value, &bufinfo, MP_BUFFER_READ);
    neopixel_hal_write(id,length,bufinfo.buf);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(mod_neopixel_write_obj, mod_neopixel_write_func);

STATIC mp_obj_t mod_neopixel_clear_func(mp_obj_t self_in) {
    mp_obj_instance_t *self = MP_OBJ_TO_PTR(self_in);

    mp_map_elem_t *n_attr = mp_map_lookup(&self->members, MP_OBJ_NEW_QSTR(MP_QSTR_n), MP_MAP_LOOKUP);
    size_t n_value = (size_t)mp_obj_get_int(n_attr->value);

    mp_map_elem_t *bpp_attr = mp_map_lookup(&self->members, MP_OBJ_NEW_QSTR(MP_QSTR_bpp), MP_MAP_LOOKUP);
    size_t bpp_value = (size_t)mp_obj_get_int(bpp_attr->value);

	mp_map_elem_t *buf_attr = mp_map_lookup(&self->members, MP_OBJ_NEW_QSTR(MP_QSTR_buf), MP_MAP_LOOKUP);
    mp_obj_array_t *pixel_array = MP_OBJ_TO_PTR(buf_attr->value);

    size_t len = n_value * bpp_value;
    mp_int_t color = 0;

    for (size_t i = 0; i < len; ++i) {
        ((uint8_t *)pixel_array->items)[i] = color;
    }

    mod_neopixel_write_func(self_in);

    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(mod_neopixel_clear_obj, mod_neopixel_clear_func);

STATIC const mp_rom_map_elem_t neopixel_module_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR___getitem__), MP_ROM_PTR(&mod_neopixel___get_item___obj) },
    { MP_ROM_QSTR(MP_QSTR___init__), MP_ROM_PTR(&mod_neopixel___init___obj) },
    { MP_ROM_QSTR(MP_QSTR___len__), MP_ROM_PTR(&mod_neopixel___len___obj) },
    { MP_ROM_QSTR(MP_QSTR___module__), MP_OBJ_NEW_QSTR(MP_QSTR_neopixel) },
    { MP_ROM_QSTR(MP_QSTR___qualname__), MP_OBJ_NEW_QSTR(MP_QSTR_NeoPixel) },
    { MP_ROM_QSTR(MP_QSTR___setitem__), MP_ROM_PTR(&mod_neopixel___set_item___obj) },
    { MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&mod_neopixel_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mod_neopixel_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_fill), MP_ROM_PTR(&mod_neopixel_fill_obj) },
    { MP_ROM_QSTR(MP_QSTR_show), MP_ROM_PTR(&mod_neopixel_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_ORDER),MP_ROM_PTR(&mod_neopixel_ORDER_obj) },
};

STATIC MP_DEFINE_CONST_DICT(neopixel_module_locals_dict, neopixel_module_locals_dict_table);

const mp_obj_type_t mod_NeoPixel_type = {
    { &mp_type_type },
    .attr = mp_obj_NeoPixel_attr,
    .locals_dict = (mp_obj_dict_t*)&neopixel_module_locals_dict,
    .make_new = mp_obj_NeoPixel_make_new,
    .name = MP_QSTR_NeoPixel,
    .print = NeoPixel_print,
    .subscr = NeoPixel_subscr,
    .unary_op = NeoPixel_unary_op,

};

//Module globals

STATIC const mp_rom_map_elem_t neopixel_module_globals_table[] = {
	{ MP_ROM_QSTR(MP_QSTR___name__), MP_OBJ_NEW_QSTR(MP_QSTR_neopixel) },
	{ MP_ROM_QSTR(MP_QSTR_ws2812_write), MP_ROM_PTR(&microbit_ws2812_write_obj) },
	{ MP_ROM_QSTR(MP_QSTR_NeoPixel), (mp_obj_t)&mod_NeoPixel_type },
};

STATIC MP_DEFINE_CONST_DICT(neopixel_module_globals, neopixel_module_globals_table);

const mp_obj_module_t neopixel_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&neopixel_module_globals,
};
