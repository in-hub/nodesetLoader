/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 *    Copyright 2020 (c) Matthias Konnerth
 */

#include <open62541.h>

#include "RefServiceImpl.h"
#include "NodesetLoader/NodesetLoader.h"

#include <assert.h>
#include <stdlib.h>

struct RefContainer
{
    size_t size;
    UA_NodeId *ids;
};
typedef struct RefContainer RefContainer;

struct RefServiceImpl
{
    RefContainer hierachicalRefs;
    RefContainer nonHierachicalRefs;
    RefContainer hasTypeDefRefs;
};

static void
RefContainer_clear(RefContainer *c) {
    for (UA_NodeId *id = c->ids; id != c->ids + c->size; id++) {
        UA_NodeId_clear(id);
    }
    free(c->ids);
}

typedef struct RefServiceImpl RefServiceImpl;

typedef void (*browseFnc)(RefServiceImpl *impl, const UA_NodeId id);

static void iterate(UA_Server *server, const UA_NodeId *startId, browseFnc fnc,
                    RefServiceImpl *impl)
{
    UA_BrowseDescription bd;
    UA_BrowseDescription_init(&bd);
    bd.browseDirection = UA_BROWSEDIRECTION_FORWARD;
    bd.includeSubtypes = true;
    bd.referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_HASSUBTYPE);
    bd.resultMask = UA_BROWSERESULTMASK_BROWSENAME;
    bd.nodeId = *startId;
    bd.nodeClassMask = UA_NODECLASS_REFERENCETYPE;
    UA_BrowseResult br = UA_Server_browse(server, 100, &bd);
    if (br.statusCode == UA_STATUSCODE_GOOD)
    {
        for (UA_ReferenceDescription *rd = br.references;
             rd != br.references + br.referencesSize; rd++)
        {
            fnc(impl, rd->nodeId.nodeId);

            iterate(server, &rd->nodeId.nodeId, fnc, impl);
        }
    }
    UA_BrowseResult_clear(&br);
}

static void
addToRefs(RefContainer *refs, const UA_NodeId id)
{
    refs->ids = (UA_NodeId *)realloc(refs->ids, sizeof(UA_NodeId) * (refs->size + 1));
    UA_NodeId *newId = refs->ids + refs->size;
    UA_NodeId_copy(&id, newId);
    refs->size++;
}

static void addToHierachicalRefs(RefServiceImpl *impl, const UA_NodeId id)
{
    addToRefs(&impl->hierachicalRefs, id);
}

static void addToNonHierachicalRefs(RefServiceImpl *impl, const UA_NodeId id)
{
    addToRefs(&impl->nonHierachicalRefs, id);
}

static void addToHasTypeDefRefs(RefServiceImpl *impl, const UA_NodeId id)
{
    addToRefs(&impl->hasTypeDefRefs, id);
}

static void getRefs(UA_Server *server, RefServiceImpl *impl,
                    const UA_NodeId startId, browseFnc fn)
{
    iterate(server, &startId, fn, impl);
}

static bool
isInContainer(const RefContainer c, const NL_Reference *ref) {
    for (const UA_NodeId *id = c.ids; id != c.ids + c.size; id++)
    {
        if (UA_NodeId_equal(&ref->refType, id))
            return true;
    }
    return false;
}

static bool isNonHierachicalRef(const RefServiceImpl *service,
                                const NL_Reference *ref)
{
    return isInContainer(service->nonHierachicalRefs, ref);
}

static bool isHierachicalRef(const RefServiceImpl *service,
                                   const NL_Reference *ref)
{
    return isInContainer(service->hierachicalRefs, ref);
}

static bool isTypeDefRef(const RefServiceImpl *service, const NL_Reference *ref)
{
    return isInContainer(service->hasTypeDefRefs, ref);
}

static void addnewRefType(RefServiceImpl *service, NL_ReferenceTypeNode *node)
{
    NL_Reference *ref = node->hierachicalRefs;
    bool isHierachical = false;
    while (ref) {
        if (!ref->isForward) {
            for (size_t i = 0; i < service->hierachicalRefs.size; i++) {
                if (UA_NodeId_equal(&service->hierachicalRefs.ids[i], &ref->target)) {
                    addToRefs(&service->hierachicalRefs, node->id);
                    isHierachical = true;
                }
            }
            for (size_t i = 0; i < service->hasTypeDefRefs.size; i++) {
                if (UA_NodeId_equal(&service->hasTypeDefRefs.ids[i], &ref->target))
                    addToRefs(&service->hasTypeDefRefs, node->id);
            }
        }
        ref = ref->next;
    }
    if (!isHierachical)
        addToRefs(&service->nonHierachicalRefs, node->id);
}

NL_ReferenceService *RefServiceImpl_new(struct UA_Server *server)
{
    RefServiceImpl *impl = (RefServiceImpl *)calloc(1, sizeof(RefServiceImpl));
    if (!impl)
    {
        return NULL;
    }
    UA_NodeId hierachicalRoot =
        UA_NODEID_NUMERIC(0, UA_NS0ID_HIERARCHICALREFERENCES);
    UA_NodeId nonHierachicalRoot =
        UA_NODEID_NUMERIC(0, UA_NS0ID_NONHIERARCHICALREFERENCES);
    UA_NodeId hasTypeDefRoot = UA_NODEID_NUMERIC(0, UA_NS0ID_HASTYPEDEFINITION);
    addToRefs(&impl->hierachicalRefs, hierachicalRoot);
    addToRefs(&impl->nonHierachicalRefs, nonHierachicalRoot);
    addToRefs(&impl->hasTypeDefRefs, hasTypeDefRoot);
    getRefs(server, impl, hierachicalRoot, addToHierachicalRefs);
    getRefs(server, impl, nonHierachicalRoot, addToNonHierachicalRefs);
    getRefs(server, impl, hasTypeDefRoot, addToHasTypeDefRefs);

    NL_ReferenceService *refService = (NL_ReferenceService *)calloc(1, sizeof(NL_ReferenceService));
    if (!refService)
    {
        RefContainer_clear(&impl->hierachicalRefs);
        RefContainer_clear(&impl->nonHierachicalRefs);
        RefContainer_clear(&impl->hasTypeDefRefs);
        free(impl);
        return NULL;
    }
    refService->context = impl;
    refService->addNewReferenceType =
        (RefService_addNewReferenceType)addnewRefType;
    refService->isHierachicalRef =
        (RefService_isRefHierachical)isHierachicalRef;
    refService->isNonHierachicalRef =
        (RefService_isRefNonHierachical)isNonHierachicalRef;
    refService->isHasTypeDefRef = (RefService_isHasTypeDefRef)isTypeDefRef;
    return refService;
}

void RefServiceImpl_delete(NL_ReferenceService *service)
{
    RefServiceImpl *impl = (RefServiceImpl *)service->context;
    RefContainer_clear(&impl->hierachicalRefs);
    RefContainer_clear(&impl->nonHierachicalRefs);
    RefContainer_clear(&impl->hasTypeDefRefs);
    free(impl);
    free(service);
}
