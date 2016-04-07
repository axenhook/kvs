/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*******************************************************************************

            Copyright(C), 2016~2019, axen.hook@foxmail.com
********************************************************************************
File Name: INDEX_OBJECT_MANAGER.C
Author   : axen.hook
Version  : 1.00
Date     : 02/Mar/2016
Description: 
Function List: 
    1. ...: 
History: 
    Version: 1.00  Author: axen.hook  Date: 02/Mar/2016
--------------------------------------------------------------------------------
    1. Primary version
*******************************************************************************/
#include "index_if.h"

MODULE(PID_INDEX);
#include "os_log.h"

int32_t compare_object2(const void *objid, object_info_t *obj_info)
{
    if ((*(uint64_t *)objid) > obj_info->objid)
    {
        return 1;
    }
    else if ((*(uint64_t *)objid) == obj_info->objid)
    {
        return 0;
    }

    return -1;
}

int32_t compare_cache1(const ifs_block_cache_t *cache, const ifs_block_cache_t *cache_node)
{
    if (cache->vbn > cache_node->vbn)
    {
        return 1;
    }
    
    if (cache->vbn < cache_node->vbn)
    {
        return -1;
    }

    return 0;
}

void init_attr(object_info_t *obj_info, uint64_t inode_no)
{
    obj_info->attr_record = INODE_GET_ATTR_RECORD(obj_info->inode);
    obj_info->root_ibc.vbn = inode_no;
    obj_info->root_ibc.ib = (block_head_t *)obj_info->attr_record->content;
}

int32_t get_object_info(index_handle_t *index, uint64_t objid, object_info_t **obj_info_out)
{
    object_info_t *obj_info = NULL;
    
    obj_info = (object_info_t *)OS_MALLOC(sizeof(object_info_t));
    if (NULL == obj_info)
    {
        LOG_ERROR("Allocate memory failed. size(%d)\n", (uint32_t)sizeof(object_info_t));
        return -INDEX_ERR_ALLOCATE_MEMORY;
    }

    memset(obj_info, 0, sizeof(object_info_t));
    obj_info->obj_ref_cnt = 0;
    obj_info->index = index;
    obj_info->objid = objid;
    
    dlist_init_head(&obj_info->obj_hnd_list);
    OS_RWLOCK_INIT(&obj_info->obj_hnd_lock);
    
    OS_RWLOCK_INIT(&obj_info->attr_lock);
    
    avl_create(&obj_info->caches, (int (*)(const void *, const void*))compare_cache1, sizeof(ifs_block_cache_t),
        OS_OFFSET(ifs_block_cache_t, obj_entry));
    OS_RWLOCK_INIT(&obj_info->caches_lock);
    
    OS_RWLOCK_INIT(&obj_info->obj_lock);

    avl_add(&index->obj_list, obj_info);

    *obj_info_out = obj_info;
    
    LOG_INFO("init object info finished. objid(%lld)\n", obj_info->objid);

    return 0;
}

void put_object_info(object_info_t *obj_info)
{
    LOG_INFO("destroy object info start. objid(%lld)\n", obj_info->objid);

    avl_remove(&obj_info->index->obj_list, obj_info);

    release_obj_all_cache(obj_info);
    avl_destroy(&obj_info->caches);
    
    OS_RWLOCK_DESTROY(&obj_info->caches_lock);
    OS_RWLOCK_DESTROY(&obj_info->attr_lock);
    OS_RWLOCK_DESTROY(&obj_info->obj_hnd_lock);
    OS_RWLOCK_DESTROY(&obj_info->obj_lock);

    OS_FREE(obj_info);
}

int32_t get_object_handle(object_info_t *obj_info, object_handle_t **obj_out)
{
    int32_t ret = 0;
    object_handle_t *obj = NULL;

    obj = (object_handle_t *)OS_MALLOC(sizeof(object_handle_t));
    if (NULL == obj)
    {
        LOG_ERROR("Allocate memory failed. size(%d)\n", (uint32_t)sizeof(object_handle_t));
        return -INDEX_ERR_ALLOCATE_MEMORY;
    }

    memset(obj, 0, sizeof(object_handle_t));
    obj->obj_info = obj_info;
    obj->index = obj_info->index;

    dlist_add_tail(&obj_info->obj_hnd_list, &obj->entry);
    obj_info->obj_ref_cnt++;
    
    *obj_out = obj;

    return 0;
}

void put_object_handle(object_handle_t *obj)
{
    obj->obj_info->obj_ref_cnt--;
    dlist_remove_entry(&obj->obj_info->obj_hnd_list, &obj->entry);
    OS_FREE(obj);

    return;
}

int32_t recover_obj_inode(object_info_t *obj_info, uint64_t inode_no)
{
    int32_t ret;
    ifs_block_cache_t *cache;
    
    ret = index_block_read2(obj_info, inode_no, INODE_MAGIC, &cache);
    if (0 > ret)
    {
        LOG_ERROR("Read inode failed. ret(%d)\n", ret);
        return ret;
    }

    //memcpy(&obj_info->inode, cache->ib, cache->ib->alloc_size);

    obj_info->inode_cache = cache;
	obj_info->inode = (inode_record_t *)obj_info->inode_cache->ib;
    obj_info->inode_no = inode_no;
    strncpy(obj_info->obj_name, obj_info->inode->name, obj_info->inode->name_size);
    init_attr(obj_info, inode_no);
    SET_IBC_CLEAN(&obj_info->root_ibc);

    return 0;
}

void validate_obj_inode(object_info_t *obj_info)
{
    ASSERT(obj_info != NULL);

    if (!IBC_DIRTY(&obj_info->root_ibc))
    {
        return;
    }

    //cache = obj_info->inode_cache;
   // memcpy(cache->ib, &obj_info->inode, cache->ib->alloc_size);
    SET_IBC_CLEAN(&obj_info->root_ibc);
    SET_IBC_DIRTY(obj_info->inode_cache);
}


void put_all_object_handle(object_info_t *obj_info)
{
    object_handle_t *obj;
    
    while (obj_info->obj_ref_cnt != 0)
    {
        obj = OS_CONTAINER(obj_info->obj_hnd_list.head.next, object_handle_t, entry);
        put_object_handle(obj);
    }
}

void close_object(object_info_t *obj_info)
{
    put_all_object_handle(obj_info);
    validate_obj_inode(obj_info);
    put_object_info(obj_info);
}

void init_inode(inode_record_t *inode, uint64_t objid, uint64_t inode_no, uint16_t flags)
{
    attr_record_t *attr_record = NULL;

    inode->head.blk_id = INODE_MAGIC;
    inode->head.alloc_size = INODE_SIZE;
    inode->head.real_size = INODE_SIZE;
    
    inode->first_attr_off = OS_OFFSET(inode_record_t, reserved);
    
    inode->objid = objid;
    inode->base_objid = 0;
    
    inode->mode = 0;
    inode->uid = 0;
    inode->gid = 0;
    inode->size = 0;
    inode->links = 0;
    inode->ctime = 0;
    inode->atime = 0;
    inode->mtime = 0;
    
    snprintf(inode->name, OBJ_NAME_MAX_SIZE, "OBJ%lld", objid);
    inode->name_size = strlen(inode->name);

    /* init attr */
    attr_record = INODE_GET_ATTR_RECORD(inode);
    attr_record->record_size = ATTR_RECORD_SIZE;
    attr_record->flags = flags;
    if (flags & FLAG_TABLE)
    { /* table */
        init_ib((index_block_t *)&attr_record->content, INDEX_BLOCK_SMALL, ATTR_RECORD_CONTENT_SIZE);
    }
    else
    { /* data stream */
        memset(attr_record->content, 0, ATTR_RECORD_CONTENT_SIZE);
    }
}

int32_t create_object_at_inode(index_handle_t *index, uint64_t objid, uint64_t inode_no, uint16_t flags, object_handle_t **obj_out)
{
    int32_t ret;
    object_handle_t *obj;
    object_info_t *obj_info;

    ASSERT(NULL != index);
    ASSERT(NULL != obj_out);
    ASSERT(INODE_SIZE == sizeof(inode_record_t));

    ret = get_object_info(index, objid, &obj_info);
    if (ret < 0)
    {
        LOG_ERROR("get_object_info failed. ret(%d)\n", ret);
        return ret;
    }

    obj_info->inode_cache = alloc_obj_cache(obj_info, inode_no, INODE_MAGIC);
    if (obj_info->inode_cache == NULL)
    {
        put_object_info(obj_info);
        LOG_ERROR("alloc_obj_cache failed\n");
        return -INDEX_ERR_ALLOCATE_MEMORY;
    }

	obj_info->inode = (inode_record_t *)obj_info->inode_cache->ib;

    /* init inode */
    init_inode(obj_info->inode, objid, inode_no, flags);

    obj_info->inode_no = inode_no;
    strncpy(obj_info->obj_name, obj_info->inode->name, obj_info->inode->name_size);
    init_attr(obj_info, inode_no);
    SET_IBC_DIRTY(&obj_info->root_ibc);

    SET_INODE_DIRTY(obj_info);

    LOG_DEBUG("Create inode success. obj_id(%lld) vbn(%lld)\n", objid, inode_no);

    ret = get_object_handle(obj_info, &obj);
    if (ret < 0)
    {
        put_object_info(obj_info);
        LOG_ERROR("Open attr failed. objid(%lld) ret(%d)\n", objid, ret);
        return ret;
    }

    *obj_out = obj;

    return 0;
}

int32_t create_object(index_handle_t *index, uint64_t objid, uint16_t flags, object_handle_t **obj_out)
{
     int32_t ret;
     uint64_t inode_no = 0;

    ASSERT(NULL != index);
    ASSERT(NULL != obj_out);
    ASSERT(INODE_SIZE == sizeof(inode_record_t));

    /* allocate inode block */
    ret = INDEX_ALLOC_BLOCK(index, objid, &inode_no);
    if (ret < 0)
    {
        LOG_ERROR("Allocate block failed. ret(%d)\n", ret);
        return ret;
    }

    ret = create_object_at_inode(index, objid, inode_no, flags, obj_out);
    if (ret < 0)
    {
        INDEX_FREE_BLOCK(index, objid, inode_no);
        LOG_ERROR("get_object_info failed. ret(%d)\n", ret);
        return ret;
    }

    return 0;
}

int32_t open_object(index_handle_t *index, uint64_t objid, uint64_t inode_no, object_handle_t **obj_out)
{
    int32_t ret = 0;
    object_info_t *obj_info = NULL;
    object_handle_t *obj = NULL;

    ASSERT(NULL != index);
    ASSERT(NULL != obj_out);

    ret = get_object_info(index, objid, &obj_info);
    if (ret < 0)
    {
        LOG_ERROR("Get object info resource failed. objid(%lld)\n", objid);
        return ret;
    }

    ret = recover_obj_inode(obj_info, inode_no);
    if (0 > ret)
    {
        put_object_info(obj_info);
        LOG_ERROR("Read inode failed. ret(%d)\n", ret);
        return ret;
    }

    ret = get_object_handle(obj_info, &obj);
    if (ret < 0)
    {
        put_object_info(obj_info);
        LOG_ERROR("Open attr failed. objid(%lld) ret(%d)\n", objid, ret);
        return ret;
    }

    *obj_out = obj;

    return 0;
}

uint64_t get_objid(index_handle_t *index)
{
    return 1;
}

int32_t set_object_name(object_handle_t *obj, char *name)
{
    uint32_t name_size;
    
    ASSERT(obj != NULL);
    ASSERT(name != NULL);

    name_size = strlen(name);
    if (name_size >= OBJ_NAME_MAX_SIZE)
    {
        return -INDEX_ERR_PARAMETER;
    }

    strncpy(obj->obj_info->obj_name, name, name_size);
    strncpy(obj->obj_info->inode->name, name, name_size);
    obj->obj_info->inode->name_size = name_size;

    SET_INODE_DIRTY(obj->obj_info);

    return 0;
}

int32_t index_create_object_nolock(index_handle_t *index, uint64_t objid, uint16_t flags, object_handle_t **obj_out)
{
    int32_t ret = 0;
    object_handle_t *obj = NULL;
    uint16_t name_size = 0;
    avl_index_t where = 0;
    object_info_t *obj_info;

    ASSERT(NULL != index);
    ASSERT(!OBJID_IS_INVALID(objid));
    ASSERT(NULL != obj_out);

    if (objid < RESERVED_OBJ_ID)
    {
        LOG_INFO("objid(%lld) should be larger than %d\n", objid, RESERVED_OBJ_ID);
        return -INDEX_ERR_OBJ_ID_INVALID;
    }

    LOG_INFO("Create the obj start. objid(%lld)\n", objid);

    obj_info = avl_find(&index->obj_list, (avl_find_fn_t)compare_object2, &objid, &where);
    if (NULL != obj_info)
    {
        LOG_ERROR("The obj already exist. obj(%p) objid(%lld) ret(%d)\n", obj, objid, ret);
        return -INDEX_ERR_OBJ_EXIST;
    }

    ret = search_key_internal(index->id_obj, &objid, sizeof(uint64_t), NULL, 0);
    if (0 <= ret)
    {
        LOG_ERROR("The obj already exist. obj(%p) objid(%lld) ret(%d)\n", obj, objid, ret);
        return -INDEX_ERR_OBJ_EXIST;
    }

    if (-INDEX_ERR_KEY_NOT_FOUND != ret)
    {
        LOG_ERROR("Search key failed. objid(%lld) ret(%d)\n", objid, ret);
        return ret;
    }

    ret = create_object(index, objid, flags, &obj);
    if (ret < 0)
    {
        LOG_ERROR("Create obj failed. objid(%lld) ret(%d)\n", objid, ret);
        return ret;
    }

    ret = index_insert_key_nolock(index->id_obj, &objid, os_u64_size(objid),
        &obj->obj_info->inode_no, os_u64_size(obj->obj_info->inode_no));
    if (ret < 0)
    {
        LOG_ERROR("Insert obj failed. obj(%p) objid(%lld) ret(%d)\n",
            obj, objid, ret);
        (void)INDEX_FREE_BLOCK(obj->index, obj->obj_info->objid, obj->obj_info->inode_no);
        close_object(obj->obj_info);
        return ret;
    }
    
    LOG_INFO("Create the obj success. objid(%lld) obj(%p) index_name(%s)\n",
        objid, obj, index->name);

    *obj_out = obj;
    
    return 0;
}    

int32_t index_create_object(index_handle_t *index, uint64_t objid, uint16_t flags, object_handle_t **obj)
{
    int32_t ret = 0;

    if ((NULL == index) || (OBJID_IS_INVALID(objid || (NULL == obj))))
    {
        LOG_ERROR("Invalid parameter. index(%p) objid(%lld) obj(%p)\n", index, objid, obj);
        return -INDEX_ERR_PARAMETER;
    }
    
    OS_RWLOCK_WRLOCK(&index->index_lock);
    ret = index_create_object_nolock(index, objid, flags, obj);
    OS_RWLOCK_WRUNLOCK(&index->index_lock);
    
    return ret;
}    

int32_t index_open_object_nolock(index_handle_t *index, uint64_t objid, uint32_t open_flags, object_handle_t **obj_out)
{
    int32_t ret = 0;
    object_handle_t *obj = NULL;
    uint64_t inode_no = 0;
    avl_index_t where = 0;
    object_handle_t *id_obj;
    object_info_t *obj_info = NULL;

    ASSERT(NULL != index);
    ASSERT(NULL != obj_out);

    LOG_INFO("Open the obj. objid(%lld)\n", objid);

    obj_info = avl_find(&index->obj_list, (avl_find_fn_t)compare_object2, &objid, &where);
    if (NULL != obj_info)
    {
        ret = get_object_handle(obj_info, &obj);
        if (ret < 0)
        {
            LOG_ERROR("get_object_handle failed. objid(%lld) ret(%d)\n", objid, ret);
            return ret;
        }

        *obj_out = obj;
        LOG_WARN("The obj obj_ref_cnt inc. obj(%p) obj_ref_cnt(%d) objid(%lld)\n",
            obj_info, obj_info->obj_ref_cnt, objid);
        return 0;
    }

    id_obj = index->id_obj;
    
    ret = search_key_internal(id_obj, &objid, sizeof(uint64_t), NULL, 0);
    if (0 > ret)
    {
        LOG_DEBUG("Search for obj failed. obj(%p) objid(%lld) ret(%d)\n", obj, objid, ret);
        return ret;
    }
    
    inode_no = os_bstr_to_u64(GET_IE_VALUE(id_obj->ie), id_obj->ie->value_len);

    ret = open_object(index, objid, inode_no, &obj);
    if (0 > ret)
    {
        LOG_ERROR("Open obj failed. objid(%lld) ret(%d)\n", objid, ret);
        return ret;
    }

    LOG_INFO("Open the obj success. index(%p) objid(%lld) obj(%p)\n",
        index, objid, obj);

    *obj_out = obj;

    return 0;
}      

int32_t index_open_object(index_handle_t *index, uint64_t objid, object_handle_t **obj)
{
    int32_t ret = 0;

    if ((NULL == index) || (NULL == obj))
    {
        LOG_ERROR("Invalid parameter. index(%p) obj(%p)\n", index, obj);
        return -INDEX_ERR_PARAMETER;
    }

    OS_RWLOCK_WRLOCK(&index->index_lock);
    ret = index_open_object_nolock(index, objid, 0, obj);
    OS_RWLOCK_WRUNLOCK(&index->index_lock);

    return ret;
}      

object_handle_t *index_get_object_handle(index_handle_t *index, uint64_t objid)
{
    object_handle_t *tmp_obj = NULL;
    avl_index_t where = 0;

    ASSERT(NULL != index);
    ASSERT(!OBJID_IS_INVALID(objid));

    OS_RWLOCK_RDLOCK(&index->index_lock);
    tmp_obj = avl_find(&index->obj_list, (avl_find_fn_t)compare_object2, &objid, &where);
    OS_RWLOCK_RDLOCK(&index->index_lock);

    return tmp_obj;
}

int32_t index_close_object_nolock(object_handle_t *obj)
{
    object_info_t *obj_info = NULL;
    
    if (NULL == obj)
    {
        LOG_ERROR("Invalid parameter. obj(%p)\n", obj);
        return -INDEX_ERR_PARAMETER;
    }
    
    obj_info = obj->obj_info;
    if (NULL == obj_info)
    {
        LOG_ERROR("Invalid object info. obj(%p)\n", obj);
        return -INDEX_ERR_PARAMETER;
    }

    OS_RWLOCK_WRLOCK(&obj_info->obj_hnd_lock);
    
    if (obj_info->obj_ref_cnt == 0)
    {
        OS_RWLOCK_WRUNLOCK(&obj_info->obj_hnd_lock);
        LOG_EMERG("Too many times put object info. objid(%lld)\n", obj_info->objid);
        return -INDEX_ERR_MANY_TIMES_PUT;
    }

    put_object_handle(obj);

    if (obj_info->obj_ref_cnt == 0) // decrease to 0
    {
        close_object(obj_info);
    }
    else
    {
        OS_RWLOCK_WRUNLOCK(&obj_info->obj_hnd_lock);
    }
    
	return 0;
}     

int32_t index_close_object(object_handle_t *obj)
{
    int32_t ret = 0;
	index_handle_t *index;

    if (NULL == obj)
    {
        LOG_ERROR("Invalid parameter. obj(%p)\n", obj);
        return -INDEX_ERR_PARAMETER;
    }

	index = obj->index;
    
    OS_RWLOCK_WRLOCK(&index->index_lock);
    ret = index_close_object_nolock(obj);
    OS_RWLOCK_WRUNLOCK(&index->index_lock);
    
    return ret;
}     

int32_t index_delete_object(index_handle_t *index, uint64_t objid)
{
    return 0;
}

int32_t index_rename_object(object_handle_t *obj, const char *new_obj_name)
{
    return 0;
}

