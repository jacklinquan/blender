/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2022 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup sequencer
 */

#include <string.h>

#include "MEM_guardedalloc.h"

#include "DNA_listBase.h"
#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"

#include "BLI_blenlib.h"

#include "SEQ_iterator.h"
#include "SEQ_relations.h"
#include "SEQ_sequencer.h"

ListBase *SEQ_channels_active_get(Editing *ed)
{
  return ed->active_channels;
}

void SEQ_channels_active_set(Editing *ed, ListBase *channels)
{
  ed->active_channels = channels;
}

void SEQ_channels_ensure(ListBase *channels)
{
  /* Allocate channels. Channel 0 is never used, but allocated to prevent off by 1 issues. */
  for (int i = 0; i < MAXSEQ + 1; i++) {
    SeqTimelineChannel *channel = MEM_callocN(sizeof(SeqTimelineChannel), "seq timeline channel");
    BLI_snprintf(channel->name, sizeof(channel->name), "channel %d", i);
    BLI_addtail(channels, channel);
  }
}

void SEQ_channels_duplicate(ListBase *channels_dst, ListBase *channels_src)
{
  LISTBASE_FOREACH (SeqTimelineChannel *, channel, channels_src) {
    SeqTimelineChannel *channel_duplicate = MEM_dupallocN(channel);
    BLI_addtail(channels_dst, channel_duplicate);
  }
}

SeqTimelineChannel *SEQ_channel_get_by_index(const ListBase *channels, const int channel_index)
{
  return BLI_findlink(channels, channel_index);
}

char *SEQ_channel_name_get(ListBase *channels, const int channel_index)
{
  SeqTimelineChannel *channel = SEQ_channel_get_by_index(channels, channel_index);
  return channel->name;
}

bool SEQ_channel_is_locked(const SeqTimelineChannel *channel)
{
  return (channel->flag & SEQ_CHANNEL_LOCK) != 0;
}

bool SEQ_channel_is_muted(const SeqTimelineChannel *channel)
{
  return (channel->flag & SEQ_CHANNEL_MUTE) != 0;
}
