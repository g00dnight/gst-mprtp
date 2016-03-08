/* GStreamer Scheduling tree
 * Copyright (C) 2015 Balázs Kreith (contact: balazs.kreith@gmail.com)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/rtp/gstrtpbuffer.h>
#include <gst/rtp/gstrtcpbuffer.h>
#include "streamsplitter.h"
#include "mprtpspath.h"
#include <math.h>


GST_DEBUG_CATEGORY_STATIC (stream_splitter_debug_category);
#define GST_CAT_DEFAULT stream_splitter_debug_category


#define THIS_READLOCK(this) g_rw_lock_reader_lock(&this->rwmutex)
#define THIS_READUNLOCK(this) g_rw_lock_reader_unlock(&this->rwmutex)
#define THIS_WRITELOCK(this) g_rw_lock_writer_lock(&this->rwmutex)
#define THIS_WRITEUNLOCK(this) g_rw_lock_writer_unlock(&this->rwmutex)

/* class initialization */
G_DEFINE_TYPE (StreamSplitter, stream_splitter, G_TYPE_OBJECT);

typedef struct _Subflow Subflow;


struct _Subflow
{
  guint8      id;
  MPRTPSPath *path;
  gint32      sent_bytes;
  gint32      sending_target;
  gint        weight;
  gboolean    key_path;
  guint32     target_rate;
  gboolean    valid;
  gboolean    mark2remove;
};

struct _SchNode
{
  SchNode *left;
  SchNode *right;
  Subflow *subflow;
  gint32   sent_bytes;
  gboolean has_keynode;
};

typedef struct{
  gint32 nc_sum;
  gint32 mc_sum;
  gint32 c_sum;
}PriorData;

typedef struct{
  gint32 valid;
  gint32 total;
}SumData;

typedef struct{
  guint16 total_weight;
  gint32  valid_sum;
}WeightData;

typedef struct{
  guint16 remained;
  SchNode *root;
  guint8   key_flag;
}CreateData;

#define _now(this) gst_clock_get_time (this->sysclock)
//----------------------------------------------------------------------
//-------- Private functions belongs to Scheduler tree object ----------
//----------------------------------------------------------------------
//Functions realted to stream_splitter
static void
stream_splitter_finalize (
    GObject * object);


static void
_iterate_subflows(
    StreamSplitter *this,
    void(*iterator)(Subflow *, gpointer),
    gpointer data);


static void
_check_pathes(
    Subflow *subflow,
    gpointer data);


static void
_validate_sending_rates(
    Subflow *subflow,
    gpointer data);


static void
_setup_sending_weights(
    Subflow *subflow,
    gpointer data);


static void
_create_nodes(
    Subflow *subflow,
    gpointer data);


static SchNode *
_tree_ctor (
    StreamSplitter *this,
    guint8 key_pathes);


//Functions related to tree
static gpointer
_schnode_ctor (void);


static void
_schtree_insert (SchNode ** node,
                 Subflow * subflow,
                 gint *change,
                 gint level_value);


static void
_schnode_rdtor (
    StreamSplitter *this,
    SchNode * node);


static Subflow *
schtree_get_next (
    SchNode * root,
    guint32 bytes_to_send,
    gboolean key_restriction);


static void
stream_splitter_run (
    void *data);


static Subflow *
make_subflow (
    MPRTPSPath * path);


static MPRTPSPath *
_get_next_path (
    StreamSplitter * this,
    GstRTPBuffer * rtp);



static void _print_tree (SchNode * node, gint top, gint level)
{
  gint i;
  if (node == NULL) {
    return;
  }
  for (i = 0; i < level; ++i)
    g_print ("--");
  if (node->subflow != NULL) {
    g_print ("%d->%d:%d (K:%d) (L:%p,R:%p)\n",
             top >> level,
             node->subflow->id,
             node->sent_bytes,
             node->has_keynode,
             node->left,
             node->right);
  } else {
    g_print ("%d->C:%d\n", top >> level, node->sent_bytes);
  }
  _print_tree (node->left, top, level + 1);
  _print_tree (node->right, top, level + 1);
}

//----------------------------------------------------------------------
//---- Private function implementations to Stream Dealer object --------
//----------------------------------------------------------------------

void
stream_splitter_class_init (StreamSplitterClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  gobject_class->finalize = stream_splitter_finalize;

  GST_DEBUG_CATEGORY_INIT (stream_splitter_debug_category, "stream_splitter", 0,
      "Stream Splitter");

}

void
stream_splitter_finalize (GObject * object)
{
  StreamSplitter *this = STREAM_SPLITTER (object);
  g_hash_table_destroy (this->subflows);
  gst_task_stop (this->thread);
  gst_task_join (this->thread);
  gst_object_unref (this->thread);
  g_object_unref(this->incoming_bytes);
  g_object_unref (this->sysclock);
}


void
stream_splitter_init (StreamSplitter * this)
{
  this->new_path_added = FALSE;
  this->changes_are_committed = FALSE;
  this->path_is_removed = FALSE;
  this->sysclock = gst_system_clock_obtain ();
  this->active_subflow_num = 0;
  this->subflows = g_hash_table_new_full (NULL, NULL, NULL, g_free);
  this->separation_is_possible = FALSE;
  this->first_delta_flag = TRUE;
  this->thread = gst_task_new (stream_splitter_run, this, NULL);
  this->incoming_bytes = make_numstracker(1<<15, GST_SECOND);
  this->trash = g_queue_new();
//    this->splitting_mode = MPRTP_STREAM_FRAME_BASED_SPLITTING;
  numstracker_reset(this->incoming_bytes);
  g_rw_lock_init (&this->rwmutex);
  g_rec_mutex_init (&this->thread_mutex);

  gst_task_set_lock (this->thread, &this->thread_mutex);
  gst_task_start (this->thread);


}

void
stream_splitter_add_path (StreamSplitter * this, guint8 subflow_id,
    MPRTPSPath * path, gint32 sending_rate)
{
  Subflow *lookup_result;
  THIS_WRITELOCK (this);
  lookup_result =
      (Subflow *) g_hash_table_lookup (this->subflows,
      GINT_TO_POINTER (subflow_id));
  if (lookup_result != NULL) {
    GST_WARNING_OBJECT (this, "The requested add operation can not be done "
        "due to duplicated subflow id (%d)", subflow_id);
    goto exit;
  }
  lookup_result = make_subflow (path);
  g_hash_table_insert (this->subflows, GINT_TO_POINTER (subflow_id),
      lookup_result);
  this->new_path_added = TRUE;
  lookup_result->sending_target = sending_rate;
  lookup_result->id = subflow_id;
  ++this->active_subflow_num;
  GST_DEBUG ("Subflow is added, the actual number of subflow is: %d",
      this->active_subflow_num);
exit:
  THIS_WRITEUNLOCK (this);
}

void
stream_splitter_rem_path (StreamSplitter * this, guint8 subflow_id)
{
  Subflow *lookup_result;
  THIS_WRITELOCK (this);
  lookup_result =
      (Subflow *) g_hash_table_lookup (this->subflows,
      GINT_TO_POINTER (subflow_id));
  if (lookup_result == NULL) {
    GST_WARNING_OBJECT (this, "The requested remove operation can not be done "
        "due to not existed subflow id (%d)", subflow_id);
    goto exit;
  }
  if(g_queue_find(this->trash, lookup_result)){
    goto exit;
  }
  g_queue_push_head(this->trash, lookup_result);
  //g_hash_table_remove (this->subflows, GINT_TO_POINTER (subflow_id));
  this->path_is_removed = TRUE;
  --this->active_subflow_num;
  GST_DEBUG ("Subflow is marked to be removed, the actual number of subflow is: %d",
      this->active_subflow_num);
exit:
  THIS_WRITEUNLOCK (this);
}


void
stream_splitter_setup_sending_target (StreamSplitter * this, guint8 subflow_id,
    gint32 sending_target)
{
  Subflow *subflow;
  THIS_WRITELOCK (this);
  subflow =
      (Subflow *) g_hash_table_lookup (this->subflows,
      GINT_TO_POINTER (subflow_id));
  if (subflow == NULL) {
    GST_WARNING_OBJECT (this,
        "The requested setup bid operation can not be done "
        "due to not existed subflow id (%d)", subflow_id);
    goto exit;
  }
  g_print("setup %d sending rate for subflow %d\n", sending_target, subflow_id);
  subflow->sending_target = sending_target;
exit:
  THIS_WRITEUNLOCK (this);
}

gint32 stream_splitter_get_encoder_rate(StreamSplitter* this)
{
  gint64 result = 0;
  THIS_READLOCK(this);
  numstracker_get_stats(this->incoming_bytes, &result);
  THIS_READUNLOCK(this);
  return result * 8;
}

void stream_splitter_set_monitor_payload_type(StreamSplitter *this, guint8 payload_type)
{
  THIS_WRITELOCK(this);
  this->monitor_payload_type = payload_type;
  THIS_WRITEUNLOCK(this);
}

gdouble stream_splitter_get_sending_target(StreamSplitter* this, guint8 subflow_id)
{
  Subflow *subflow;
  gdouble result = 0.;
  THIS_READLOCK(this);
  subflow =
        (Subflow *) g_hash_table_lookup (this->subflows,
        GINT_TO_POINTER (subflow_id));
  if(!subflow) goto done;
  result = subflow->sending_target;
done:
  THIS_READUNLOCK(this);
  return result;
}

void
stream_splitter_commit_changes (StreamSplitter * this)
{
  THIS_WRITELOCK (this);
  this->changes_are_committed = TRUE;
  THIS_WRITEUNLOCK (this);
}


MPRTPSPath *
stream_splitter_get_next_path (StreamSplitter * this, GstBuffer * buf)
{
  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  MPRTPSPath *path = NULL;
  if (G_UNLIKELY (!gst_rtp_buffer_map (buf, GST_MAP_READ, &rtp))) {
    GST_WARNING_OBJECT (this, "The RTP packet is not readable");
    goto exit;
  }
  THIS_WRITELOCK (this);
  if (this->tree == NULL) {
    //Somewhere, over the rainbow a path may exist
    GST_WARNING_OBJECT (this, "No active subflow");
    goto done;
  }
  path = _get_next_path (this, &rtp);
done:
  gst_rtp_buffer_unmap (&rtp);
  THIS_WRITEUNLOCK (this);
exit:
  return path;
}


void
stream_splitter_run (void *data)
{
  StreamSplitter *this;
  GstClockID clock_id;
  GstClockTime next_scheduler_time;
  GstClockTime interval;
  gdouble rand;
  PriorData pdata;
  guint8 key_filter;

  this = STREAM_SPLITTER (data);

  THIS_WRITELOCK (this);

  numstracker_obsolate(this->incoming_bytes);
  if (!this->new_path_added &&
      !this->path_is_removed && !this->changes_are_committed) {
    goto done;
  }

  if(!this->active_subflow_num){
    _schnode_rdtor(this, this->tree);
    this->tree = NULL;
    goto done;
  }

  while(this->path_is_removed && !g_queue_is_empty(this->trash)){
    Subflow *candidate;
    candidate = g_queue_pop_head(this->trash);
    g_hash_table_remove (this->subflows, GINT_TO_POINTER (candidate->id));
  }

//  g_print("new path: %d removed: %d changed: %d\n", this->new_path_added, this->path_is_removed, this->changes_are_committed);
  pdata.c_sum = pdata.mc_sum = pdata.nc_sum = 0;
  _iterate_subflows(this, _check_pathes, &pdata);
  this->sending_target = pdata.c_sum + pdata.mc_sum + pdata.nc_sum;

  if(pdata.nc_sum) key_filter = MPRTPS_PATH_FLAG_NON_CONGESTED | MPRTPS_PATH_FLAG_NON_LOSSY;
  else if(pdata.mc_sum) key_filter = MPRTPS_PATH_FLAG_NON_CONGESTED;
  else key_filter = MPRTPS_PATH_FLAG_ACTIVE;

  this->next_tree = _tree_ctor(this, key_filter);

  DISABLE_LINE _print_tree(this->next_tree, SCHTREE_MAX_VALUE, 0);

  if(1 || this->tree == NULL){
    _schnode_rdtor(this, this->tree);
    this->tree = this->next_tree;
    this->next_tree = NULL;
  }

  this->new_path_added = FALSE;
  this->path_is_removed = FALSE;
  this->changes_are_committed = FALSE;


done:
  if (this->active_subflow_num > 0) {
    rand = g_random_double () * 100.;
    interval = GST_MSECOND * (100 + rand);
    next_scheduler_time = _now(this) + interval;

    GST_DEBUG_OBJECT (this, "Next scheduling interval time is %lu",
        GST_TIME_AS_MSECONDS (interval));
  } else {
    next_scheduler_time = _now(this) + GST_MSECOND * 10;
  }
  clock_id = gst_clock_new_single_shot_id (this->sysclock, next_scheduler_time);

  THIS_WRITEUNLOCK (this);

  if (gst_clock_id_wait (clock_id, NULL) == GST_CLOCK_UNSCHEDULED) {
    GST_WARNING_OBJECT (this, "The scheduler clock wait is interrupted");
  }
  gst_clock_id_unref (clock_id);
}




static void _iterate_subflows(StreamSplitter *this, void(*iterator)(Subflow *, gpointer), gpointer data)
{
  GHashTableIter iter;
  gpointer key, val;
  Subflow *subflow;

  g_hash_table_iter_init (&iter, this->subflows);
  while (g_hash_table_iter_next (&iter, (gpointer) & key, (gpointer) & val)) {
    subflow = (Subflow *) val;
    iterator(subflow, data);
  }
}



void _check_pathes(Subflow *subflow, gpointer data)
{
  PriorData  *pdata = data;
  MPRTPSPath *path = subflow->path;
  guint8      path_flags;
  if (mprtps_path_is_new (path)) {
    mprtps_path_set_not_new (path);
    mprtps_path_set_non_congested (path);
    mprtps_path_set_non_lossy (path);
  }

  path_flags = mprtps_path_get_flags (path);

  if(path_flags & MPRTPS_PATH_FLAG_NON_CONGESTED){
    if(path_flags & MPRTPS_PATH_FLAG_NON_LOSSY)
      pdata->nc_sum += subflow->sending_target;
    else
      pdata->mc_sum += subflow->sending_target;
  }else{
      pdata->c_sum += subflow->sending_target;
  }

  subflow->valid = FALSE;
}


MPRTPSPath *
_get_next_path (StreamSplitter * this, GstRTPBuffer * rtp)
{
  Subflow *subflow = NULL;
  guint32 bytes_to_send;
  gboolean dflag;

  bytes_to_send = gst_rtp_buffer_get_payload_len (rtp);
  dflag = GST_BUFFER_FLAG_IS_SET (rtp->buffer, GST_BUFFER_FLAG_DELTA_UNIT);
  if(this->first_delta_flag){
    this->first_delta_flag = TRUE;
    this->last_delta_flag = dflag;
  }else{
    if(dflag != this->last_delta_flag) this->separation_is_possible = TRUE;
    this->last_delta_flag = dflag;
  }
  if(this->separation_is_possible){
    subflow = schtree_get_next(this->tree, bytes_to_send, !dflag);
  }else{
    subflow = schtree_get_next(this->tree, bytes_to_send, FALSE);
  }

  if(gst_rtp_buffer_get_payload_type(rtp) != this->monitor_payload_type){
    numstracker_add(this->incoming_bytes, bytes_to_send);
  }

  return subflow ? subflow->path : NULL;
}


#define _setup_tree(root) _setup_node(root, 0)
static void _setup_node(SchNode * node, guint level)
{
  gboolean has_keynode = FALSE;
  gint32   sent_bytes = 0;
  if(node->subflow != NULL){
    node->has_keynode = node->subflow->key_path;
    node->sent_bytes = node->subflow->sent_bytes>>level;
    return;
  }

  if(node->left != NULL){
    _setup_node(node->left, level + 1);
    sent_bytes += node->left->sent_bytes;
    has_keynode |= node->left->has_keynode;
  }

  if(node->right != NULL){
    _setup_node(node->right, level + 1);
    sent_bytes += node->right->sent_bytes;
    has_keynode |= node->right->has_keynode;
  }

  node->has_keynode = has_keynode;
  node->sent_bytes = sent_bytes;
}


void _validate_sending_rates(Subflow *subflow, gpointer data)
{
  SumData *sumd = data;
  gdouble exp_ratio = (gdouble) subflow->sending_target / (gdouble)sumd->total;
  gdouble treshold = 1. / (gdouble) SCHTREE_MAX_VALUE;
  if(exp_ratio < treshold) return;
  subflow->valid = TRUE;
  sumd->valid += subflow->sending_target;
}


void _setup_sending_weights(Subflow *subflow, gpointer data)
{
  WeightData *wdata = data;
  gdouble weight;
  if(!subflow->valid) return;
  weight = (gdouble) subflow->sending_target / (gdouble) wdata->valid_sum;
  weight *= (gdouble) SCHTREE_MAX_VALUE;
  wdata->total_weight+=subflow->weight = weight;
}


void _create_nodes(Subflow *subflow, gpointer data)
{
  CreateData *cdata = data;
  if(!subflow->valid) return;
  if(cdata->remained){
    subflow->weight += cdata->remained;
    cdata->remained = 0;
  }
  subflow->key_path = (mprtps_path_get_flags(subflow->path) & cdata->key_flag) == cdata->key_flag;
  subflow->sent_bytes = mprtps_path_get_sent_bytes_in1s(subflow->path);
  _schtree_insert(&cdata->root, subflow, &subflow->weight, SCHTREE_MAX_VALUE);
}

SchNode *
_tree_ctor (StreamSplitter *this, guint8 key_pathes)
{
  CreateData cdata;
  WeightData wdata;
  SumData sdata;

  sdata.valid = wdata.total_weight = 0;
  cdata.root = NULL;
  sdata.total = this->sending_target;
  _iterate_subflows(this, _validate_sending_rates, &sdata);
  wdata.valid_sum = sdata.valid;
  _iterate_subflows(this, _setup_sending_weights, &wdata);
  cdata.remained = SCHTREE_MAX_VALUE - wdata.total_weight;
  cdata.key_flag = key_pathes;
  _iterate_subflows(this, _create_nodes, &cdata);

  _setup_tree(cdata.root);
  return cdata.root;
}

void
_schnode_rdtor (StreamSplitter *this,SchNode * node)
{
  if (node == NULL) {
    return;
  }
  _schnode_rdtor (this, node->left);
  _schnode_rdtor (this, node->right);
//  pointerpool_add(this->pointerpool, node);
//  g_slice_free(SchNode, node);
  g_free(node);
}

void
_schtree_insert (SchNode ** node, Subflow * subflow, gint * change,
    gint level_value)
{

  if (*node == NULL) {
    *node = _schnode_ctor ();
  }
  if ((*node)->subflow != NULL || level_value < 1) {
    goto done;
  }
  if (*change >= level_value && (*node)->left == NULL && (*node)->right == NULL) {
    *change -= level_value;
    (*node)->subflow = subflow;
    goto done;
  }
  _schtree_insert (&(*node)->left, subflow, change, level_value >> 1);
  if (*change < 1) {
    goto done;
  }
  _schtree_insert (&(*node)->right, subflow, change, level_value >> 1);

done:
//  g_print("Change: %d->%d\n", subflow->id, *change);
  return;
}

gpointer
_schnode_ctor (void)
{

//  SchNode *result = g_slice_new0(SchNode);
  SchNode *result = g_malloc0(sizeof(SchNode));
  result->left = NULL;
  result->right = NULL;
//  result->next = NULL;
  result->subflow = NULL;
  result->sent_bytes = 0;
  return result;
}


Subflow *
schtree_get_next (SchNode * root, guint32 bytes_to_send, gboolean key_restriction)
{
  Subflow *result;
  SchNode *selected, *left, *right;
  selected = root;
  while (selected->subflow == NULL) {
    left = selected->left;
    right = selected->right;
    selected->sent_bytes += bytes_to_send;
    if(key_restriction && (!left->has_keynode || !right->has_keynode)){
      selected = left->has_keynode ? left : right;
    }else{
      selected = left->sent_bytes <= right->sent_bytes ? left : right;
    }
  }
  selected->sent_bytes += bytes_to_send;
//  g_print("bytes to send: %u, frame to send: %u\n", bytes_to_send, frames_to_send);
//  g_print("selected path: %d decision value: %u\n", selected->path->id, *selected->decision_value);
  result = selected->subflow;
//  g_print("%d->", result->id);
  return result;
}

//
//
//MPRTPSPath *
//schtree_get_next (SchNode * root)
//{
//  MPRTPSPath *result;
//  SchNode *selected;
//  selected = root;
//  while (selected->path == NULL) {
//    selected->next =
//        (selected->next == selected->left) ? selected->right : selected->left;
//    selected = selected->next;
//  }
//  result = selected->path;
//  return result;
//}


Subflow *
make_subflow (MPRTPSPath * path)
{

  Subflow *result; // = g_slice_new0(Subflow);
  result = g_malloc0(sizeof(Subflow));
  result->path = path;
  return result;
}



#undef THIS_READLOCK
#undef THIS_READUNLOCK
#undef THIS_WRITELOCK
#undef THIS_WRITEUNLOCK
#undef SCHTREE_MAX_VALUE
