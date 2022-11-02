/* VkVideoParser
 * Copyright (C) 2022 Igalia, S.L.
 *     Author: Víctor Jáquez <vjaquez@igalia.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You
 * may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.  See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include "gstvkvideoparser.h"

#include <gst/check/gstharness.h>

#include "gstvkh264parse.h"
//#include "gstvkh265dec.h"

struct _GstVkVideoParser
{
  GstObject parent;
  gpointer user_data;
  VkVideoCodecOperationFlagBitsKHR codec;
  gboolean oob_pic_params;
  GstHarness *parser;
  GstBus *bus;
};

enum
{
  PROP_USER_DATA = 1,
  PROP_CODEC,
  PROP_OOB_PIC_PARAMS,
};


GST_DEBUG_CATEGORY (gst_vk_video_parser_debug);
#define GST_CAT_DEFAULT gst_vk_video_parser_debug


GstVkVideoParser::GstVkVideoParser (gpointer client, VkVideoCodecOperationFlagBitsKHR codec, gboolean oob_pic_params)
      :m_client(client),
      m_codec(codec),
      m_oob_pic_params(oob_pic_params)
{
  GST_DEBUG_CATEGORY_INIT (gst_vk_video_parser_debug, "vkvideoparser", 0, "Vulkan Video Parser");
}

GstVkVideoParser::~GstVkVideoParser()
{
  dispose();
}

void
GstVkVideoParser::processMessages ()
{
  GstMessage *msg;

  while ((msg = gst_bus_pop (this->m_bus))) {
    GST_DEBUG_OBJECT (this, "%s", GST_MESSAGE_TYPE_NAME (msg));

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_ERROR:{
        GError *err = NULL;
        char *debug = NULL;

        gst_message_parse_error (msg, &err, &debug);
        GST_ERROR_OBJECT (this, "Error: %s - %s", err->message, debug);
        g_clear_error (&err);
        g_free (debug);
        break;
      }
      case GST_MESSAGE_WARNING:{
        GError *err = NULL;
        char *debug = NULL;

        gst_message_parse_warning (msg, &err, &debug);
        GST_WARNING_OBJECT (this, "Warning: %s - %s", err->message, debug);
        g_clear_error (&err);
        g_free (debug);
        break;
      }
      case GST_MESSAGE_EOS:
        GST_DEBUG_OBJECT (this, "Got EOS");
        break;
      default:
        break;
    }

    gst_message_unref (msg);
  }
}

void GstVkVideoParser::dispose ()
{
  
  GstMessage *msg;

  gst_harness_teardown (m_harness);

  /* drain bus after bin unref */
  while ((msg = gst_bus_pop (this->m_bus))) {
    GST_DEBUG_OBJECT (this, "%s", GST_MESSAGE_TYPE_NAME (msg));
    gst_message_unref (msg);
  }

  gst_object_unref (this->m_bus);
}

void
on_gst_buffer (GstElement * element,
    GstBuffer * buf, GstPad * pad, GstVkVideoParser * parser)
{
  GST_ERROR ("Coucou buffer");
  parser->parseBuffer(buf);
}

bool GstVkVideoParser::build ()
{
  GstElement *bin, *parser, *sink;
  const char *parser_name = NULL;
  const char* src_caps_desc = NULL;
  GstPad *pad;

  if (m_codec == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT) {
    parser_name = "h264parse";
    src_caps_desc = "video/x-h264,stream-format=byte-stream";
    m_parser = new GstVkH264Parser(reinterpret_cast <VkParserVideoDecodeClient *>(m_client));
    // decoder = reinterpret_cast<GstElement*>(g_object_new (GST_TYPE_VK_H264_DEC, "user-data", m_user_data,
    //     "oob-pic-params", m_oob_pic_params, NULL));
    // g_assert (decoder);
  } else if (m_codec == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT) {
    parser_name = "h265parse";
    src_caps_desc = "video/x-h265,stream-format=byte-stream";
    // decoder = reinterpret_cast<GstElement*>(g_object_new (GST_TYPE_VK_H265_DEC, "user-data", m_user_data,
    //     "oob-pic-params", m_oob_pic_params, NULL));
    // g_assert (decoder);
  }
  else {
    return false;
  }

  parser = gst_element_factory_make (parser_name, NULL);
  sink = gst_element_factory_make ("fakesink", NULL);
  g_object_set (sink, "async", FALSE, "sync", FALSE, NULL);
  g_object_set (G_OBJECT (sink), "signal-handoffs", TRUE, NULL);
  g_signal_connect (sink, "handoff", G_CALLBACK (on_gst_buffer), this);

  bin = gst_bin_new (NULL);
  gst_bin_add_many (GST_BIN (bin), parser, sink, NULL);

  if (!gst_element_link_many (parser, sink, NULL)) {
    GST_WARNING_OBJECT (this, "Failed to link element");
    return false;
  }
  if ((pad = gst_bin_find_unlinked_pad (GST_BIN (bin), GST_PAD_SINK)) != NULL) {
    gst_element_add_pad (GST_ELEMENT (bin), gst_ghost_pad_new ("sink", pad));
    gst_object_unref (pad);
  }

  m_harness = gst_harness_new_with_element (bin, "sink", NULL);

  m_bus = gst_bus_new ();
  gst_element_set_bus (bin, m_bus);

  gst_object_unref (bin);

  gst_harness_set_live (m_harness, FALSE);

  gst_harness_set_src_caps_str (m_harness,
      src_caps_desc);

  gst_harness_play (m_harness);

  return true;
}



GstFlowReturn GstVkVideoParser::pushBuffer (GstBuffer * buffer)
{
  GstFlowReturn ret;

  GST_DEBUG_OBJECT (this, "Pushing buffer: %" GST_PTR_FORMAT, buffer);

  ret = gst_harness_push (m_harness, buffer);
  if (ret != GST_FLOW_OK && ret != GST_FLOW_EOS) {
    GST_WARNING_OBJECT (this, "Couldn't push buffer: %s",
        gst_flow_get_name (ret));
    return ret;
  }

  processMessages ();

  return ret;
}

GstFlowReturn GstVkVideoParser::eos ()
{
  GST_DEBUG_OBJECT (this, "Pushing EOS");

  if (!gst_harness_push_event (m_harness, gst_event_new_eos ()))
    return GST_FLOW_ERROR;

  processMessages ();

  return GST_FLOW_EOS;
}


void GstVkVideoParser::parseBuffer(GstBuffer * buffer)
{
  m_parser->parse(buffer);
}


