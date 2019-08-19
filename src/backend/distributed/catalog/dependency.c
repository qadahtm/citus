/*-------------------------------------------------------------------------
 *
 * dependency.c
 *	  Functions to reason about distributed objects and their dependencies
 *
 * Copyright (c) 2019, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_class.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_type.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "distributed/dist_catalog/dependency.h"
#include "distributed/dist_catalog/distobject.h"


static bool SupportedDependencyByCitus(const ObjectAddress *address);
static bool IsObjectAddressInList(const ObjectAddress *findAddress, List *addressList);
static bool IsObjectAddressOwnedByExtension(const ObjectAddress *target);

static void recurse_pg_depend(const ObjectAddress *target,
							  bool (*follow)(void *context, Form_pg_depend row),
							  List * (*expand)(void *context, Form_pg_depend row),
							  void (*apply)(void *context, Form_pg_depend row),
							  void *context);
static bool follow_order_object_address(void *context, Form_pg_depend pg_depend);
static bool follow_get_dependencies_for_object(void *context, Form_pg_depend pg_depend);
static void apply_add_to_target_list(void *context, Form_pg_depend pg_depend);
static List * epxand_citus_supported_types(void *context, Form_pg_depend pg_depend);


/*
 * GetDependenciesForObject returns a list of ObjectAddesses to be created in order
 * before the target object could safely be created on a worker. Some of the object might
 * already be created on a worker. It should be created in an idempotent way.
 */
List *
GetDependenciesForObject(const ObjectAddress *target)
{
	List *dependencyList = NIL;
	recurse_pg_depend(target,
					  &follow_get_dependencies_for_object,
					  &epxand_citus_supported_types,
					  &apply_add_to_target_list,
					  &dependencyList);
	return dependencyList;
}


/*
 * IsObjectAddressInList is a helper function that can check if an ObjectAddress is
 * already in a (unsorted) list of ObjectAddresses
 */
static bool
IsObjectAddressInList(const ObjectAddress *findAddress, List *addressList)
{
	ListCell *addressCell = NULL;
	foreach(addressCell, addressList)
	{
		ObjectAddress *currentAddress = (ObjectAddress *) lfirst(addressCell);

		/* equality check according as used in postgres for object_address_present */
		if (findAddress->classId == currentAddress->classId && findAddress->objectId ==
			currentAddress->objectId)
		{
			if (findAddress->objectSubId == currentAddress->objectSubId ||
				currentAddress->objectSubId == 0)
			{
				return true;
			}
		}
	}

	return false;
}


/*
 * SupportedDependencyByCitus returns whether citus has support to distribute the object
 * addressed.
 */
static bool
SupportedDependencyByCitus(const ObjectAddress *address)
{
	/*
	 * looking at the type of a object to see if we know how to create the object on the
	 * workers.
	 */
	switch (getObjectClass(address))
	{
		case OCLASS_SCHEMA:
		{
			return true;
		}

		case OCLASS_TYPE:
		{
			switch (get_typtype(address->objectId))
			{
				case TYPTYPE_ENUM:
				case TYPTYPE_COMPOSITE:
				{
					return true;
				}

				case TYPTYPE_BASE:
				{
					/*
					 * array types should be followed but not created, as they get created
					 * by the original type.
					 */
					return type_is_array(address->objectId);
				}

				default:
				{
					/* type not supported */
					return false;
				}
			}

			/*
			 * should be unreachable, break here is to make sure the function has a path
			 * without return, instead of falling through to the next block */
			break;
		}

		default:
		{
			/* unsupported type */
			return false;
		}
	}

	/*
	 * all types should have returned above, compilers complaining about a non return path
	 * indicate a bug in the above switch. Missing returns or breaking code flow should be
	 * addressed in the switch statement, preferably not here.
	 */
}


/*
 * IsObjectAddressOwnedByExtension returns whether or not the object is owned by an
 * extension. It is assumed that an object having a dependency on an extension is created
 * by that extension and therefore owned by that extension.
 */
static bool
IsObjectAddressOwnedByExtension(const ObjectAddress *target)
{
	Relation depRel = NULL;
	ScanKeyData key[2] = { 0 };
	SysScanDesc depScan = NULL;
	HeapTuple depTup = NULL;
	bool result = false;

	depRel = heap_open(DependRelationId, AccessShareLock);

	/* scan pg_depend for classid = $1 AND objid = $2 using pg_depend_depender_index */
	ScanKeyInit(&key[0], Anum_pg_depend_classid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(target->classId));
	ScanKeyInit(&key[1], Anum_pg_depend_objid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(target->objectId));
	depScan = systable_beginscan(depRel, DependDependerIndexId, true, NULL, 2, key);

	while (HeapTupleIsValid(depTup = systable_getnext(depScan)))
	{
		Form_pg_depend pg_depend = (Form_pg_depend) GETSTRUCT(depTup);
		if (pg_depend->deptype == DEPENDENCY_EXTENSION)
		{
			result = true;
			break;
		}
	}

	systable_endscan(depScan);
	relation_close(depRel, AccessShareLock);

	return result;
}


/*
 * OrderObjectAddressListInDependencyOrder given a list of ObjectAddresses return a new
 * list of the same ObjectAddresses ordered on dependency order with the objects without
 * dependencies first.
 *
 * The algortihm traveses pg_depend in a depth first order starting at the first object in
 * the provided list. By traversing depth first it will put the first dependency at the
 * head of the list with dependencies depending on them later.
 *
 * If the object is already in the list it is skipped for traversal. This happens when an
 * object was already added to the target list before it occurred in the input list.
 */
List *
OrderObjectAddressListInDependencyOrder(List *objectAddressList)
{
	List *targetList = NIL;
	ListCell *ojectAddressCell = NULL;
	foreach(ojectAddressCell, objectAddressList)
	{
		ObjectAddress *objectAddress = (ObjectAddress *) lfirst(ojectAddressCell);
		if (IsObjectAddressInList(objectAddress, targetList))
		{
			/* skip objects that are already ordered */
			continue;
		}

		recurse_pg_depend(objectAddress,
						  &follow_order_object_address,
						  &epxand_citus_supported_types,
						  &apply_add_to_target_list,
						  &targetList);

		/* all dependencies are added before, now add the current object */
		targetList = lappend(targetList, objectAddress);
	}

	return targetList;
}


/*
 * recurse_pg_depend recursively visits pg_depend entries, starting at the target
 * ObjectAddress. For every entry the follow function will be called. When follow returns
 * true it will recursively visit the dependencies for that object. recurse_pg_depend will
 * visit therefore all pg_depend entries.
 *
 * Visiting will happen in depth first order, which is useful to create or sort lists of
 * dependencies to create.
 *
 * For all pg_depend entries that should be visited the apply function will be called.
 * This function is designed to be the mutating function for the context being passed.
 * Although nothing prevents the follow function to also mutate the context.
 *
 *  - follow will be called on the way down, so the invocation order is top to bottom of
 *    the dependency tree
 *  - apply is called on the way back, so the invocation order is bottom to top. Apply is
 *    not called for entries for which follow has returned false.
 */
static void
recurse_pg_depend(const ObjectAddress *target,
				  bool (*follow)(void *context, Form_pg_depend row),
				  List * (*expand)(void *context, Form_pg_depend row),
				  void (*apply)(void *context, Form_pg_depend row),
				  void *context)
{
	Relation depRel = NULL;
	ScanKeyData key[2] = { 0 };
	SysScanDesc depScan = NULL;
	HeapTuple depTup = NULL;

	depRel = heap_open(DependRelationId, AccessShareLock);

	/* scan pg_depend for classid = $1 AND objid = $2 using pg_depend_depender_index */
	ScanKeyInit(&key[0], Anum_pg_depend_classid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(target->classId));
	ScanKeyInit(&key[1], Anum_pg_depend_objid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(target->objectId));
	depScan = systable_beginscan(depRel, DependDependerIndexId, true, NULL, 2, key);

	while (HeapTupleIsValid(depTup = systable_getnext(depScan)))
	{
		Form_pg_depend pg_depend = (Form_pg_depend) GETSTRUCT(depTup);
		ObjectAddress address = { 0 };
		ObjectAddressSet(address, pg_depend->refclassid, pg_depend->refobjid);

		if (follow == NULL || !follow(context, pg_depend))
		{
			/* skip all pg_depend entries the user didn't want to follow */
			continue;
		}

		/*
		 * recurse depth first, this makes sure we call apply for the deepest dependency
		 * first.
		 */
		recurse_pg_depend(&address, follow, expand, apply, context);

		/*
		 * the expand function will be used (when set) to add more candidates to the
		 * exploration.
		 */
		if (expand != NULL)
		{
			List *expansion = expand(context, pg_depend);
			ListCell *expandedObjectAddress = NULL;
			foreach(expandedObjectAddress, expansion)
			{
				ObjectAddress *expandedAddress = (ObjectAddress *) lfirst(
					expandedObjectAddress);
				recurse_pg_depend(expandedAddress, follow, expand, apply, context);

				if (apply != NULL)
				{
					/*
					 * create a pg_depend entry for the expanded row, simulating a normal
					 * dependency from out current target to our expanded object.
					 */
					FormData_pg_depend expanded_pg_depend = { 0 };
					expanded_pg_depend.classid = target->classId;
					expanded_pg_depend.objid = target->objectId;
					expanded_pg_depend.objsubid = target->objectSubId;

					expanded_pg_depend.refclassid = expandedAddress->classId;
					expanded_pg_depend.refobjid = expandedAddress->objectId;
					expanded_pg_depend.refobjsubid = expandedAddress->objectSubId;
					expanded_pg_depend.deptype = DEPENDENCY_NORMAL;

					apply(context, &expanded_pg_depend);
				}
			}
		}

		/* now apply changes for current entry */
		if (apply != NULL)
		{
			apply(context, pg_depend);
		}
	}

	systable_endscan(depScan);
	relation_close(depRel, AccessShareLock);
}


/*
 * follow_get_dependencies_for_object applies filters on pg_depend entries to follow all
 * objects which should be distributed before the root object can safely be created.
 *
 * Objects are only followed if all of the following checks hold true:
 *  - the object is not already in the targetList (context), if it is already in there an
 *    other object already caused the creation of this object
 *  - the object is not already marked as distributed in pg_dist_object. Objects in
 *    pg_dist_object are already available on all workers.
 *  - object is not created by an other extension. Objects created by extensions are
 *    assumed to be created on the worker when the extension is created there.
 *  - citus supports object distribution. If citus does not support the distribution of
 *    the object we will not try and distribute it.
 */
static bool
follow_get_dependencies_for_object(void *context, Form_pg_depend pg_depend)
{
	List **targetList = (List **) context;
	ObjectAddress address = { 0 };
	ObjectAddressSet(address, pg_depend->refclassid, pg_depend->refobjid);

	if (pg_depend->deptype != DEPENDENCY_NORMAL)
	{
		return false;
	}

	if (IsObjectAddressInList(&address, *targetList))
	{
		return false;
	}

	if (isObjectDistributed(&address))
	{
		return false;
	}

	if (IsObjectAddressOwnedByExtension(&address))
	{
		return false;
	}

	if (!SupportedDependencyByCitus(&address))
	{
		return false;
	}

	return true;
}


/*
 * follow_order_object_address applies filters on pg_depend entries to follow the
 * dependency tree of objects in depth first order. The filters are practically the same
 * to follow_get_dependencies_for_object, except it will follow objects that have already
 * been distributed. This is because its primary use is to order the objects from
 * pg_dist_object in dependency order.
 *
 * For completeness the list of all edges it will follow;
 *
 * Objects are only followed if all of the following checks hold true:
 *  - the object is not already in the targetList (context), if it is already in there an
 *    other object already caused the creation of this object
 *  - object is not created by an other extension. Objects created by extensions are
 *    assumed to be created on the worker when the extension is created there.
 *  - citus supports object distribution. If citus does not support the distribution of
 *    the object we will not try and distribute it.
 */
static bool
follow_order_object_address(void *context, Form_pg_depend pg_depend)
{
	List **targetList = (List **) context;
	ObjectAddress address = { 0 };
	ObjectAddressSet(address, pg_depend->refclassid, pg_depend->refobjid);

	if (pg_depend->deptype != DEPENDENCY_NORMAL)
	{
		return false;
	}

	if (IsObjectAddressInList(&address, *targetList))
	{
		return false;
	}

	if (IsObjectAddressOwnedByExtension(&address))
	{
		return false;
	}

	if (!SupportedDependencyByCitus(&address))
	{
		return false;
	}

	return true;
}


/*
 * apply_add_to_target_list is an apply function for recurse_pg_depend that will append
 * all the ObjectAddresses for pg_depend entries to the context. The context here is
 * assumed to be a (List **) to the location where all ObjectAddresses will be collected.
 */
static void
apply_add_to_target_list(void *context, Form_pg_depend pg_depend)
{
	List **targetList = (List **) context;
	ObjectAddress *address = palloc0(sizeof(ObjectAddress));
	ObjectAddressSet(*address, pg_depend->refclassid, pg_depend->refobjid);

	*targetList = lappend(*targetList, address);
}


/*
 * epxand_citus_supported_types base on supported types by citus we might want to expand
 * the list of objects to visit in pg_depend.
 *
 * An example where we want to expand is for types. Their dependencies are not captured
 * with an entry in pg_depend from their object address, but by the object address of the
 * relation describing the type.
 */
static List *
epxand_citus_supported_types(void *context, Form_pg_depend pg_depend)
{
	List *result = NIL;
	ObjectAddress *target = (ObjectAddress *) (&pg_depend->refclassid);

	switch (target->classId)
	{
		case TypeRelationId:
		{
			/*
			 * types depending on other types are not captured in pg_depend, instead they
			 * are described with their dependencies by the relation that describes the
			 * composite type.
			 */
			if (get_typtype(target->objectId) == TYPTYPE_COMPOSITE)
			{
				ObjectAddress *address = palloc0(sizeof(ObjectAddress));
				ObjectAddressSet(*address, RelationRelationId, get_typ_typrelid(
									 target->objectId));
				result = lappend(result, address);
			}
			break;
		}

		default:
		{
			/* no expansion for unsupported types */
			break;
		}
	}
	return result;
}
