/* VideoParser
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

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "dump.h"
#include "videoparser.h"

#include <vk_video/vulkan_video_codecs_common.h>
#include <vulkan/vulkan_beta.h>

#include <sys/syscall.h>
#include <unistd.h>

static VkVideoCodecOperationFlagBitsKHR codec = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT;

class PictureParameterSet : public VkParserVideoRefCountBase {
public:
    static PictureParameterSet* create()
    {
        return new PictureParameterSet();
    }

    int32_t AddRef() final
    {
        return ++m_refCount;
    }

    int32_t Release() final
    {
        uint32_t ret = --m_refCount;
        // Destroy the device if refcount reaches zero
        if (ret == 0)
            delete this;
        return ret;
    }

private:
    std::atomic<int32_t> m_refCount;

    PictureParameterSet()
        : m_refCount(0)
    {
    }
};

class Picture : public VkPicIf {
public:
    Picture()
        : m_refCount(0)
    {
    }

    void AddRef() final
    {
        assert(m_refCount >= 0);
        ++m_refCount;
    }

    void Release() final
    {
        assert(m_refCount > 0);
        int32_t ref = --m_refCount;
        if (ref == 0) {
            decodeHeight = 0;
            decodeWidth = 0;
            decodeSuperResWidth = 0;
        }
    }

    bool isAvailable() const { return m_refCount == 0; }

private:
    std::atomic<int32_t> m_refCount;
};

class VideoParserClient : public VkParserVideoDecodeClient {
public:
    VideoParserClient()
        : m_dpb(32)
    {
    }

    int32_t BeginSequence(const VkParserSequenceInfo* info) final
    {
        int32_t max = 16, conf = 1;

        fprintf(stdout, "[%lu] %s\n", syscall(SYS_gettid), __FUNCTION__);

        dump_parser_sequence_info(info);

        if (info->eCodec == VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_EXT)
            max++;

        if (info->nMinNumDecodeSurfaces > 0)
            conf += info->nMinNumDecodeSurfaces - (info->isSVC ? 3 : 1);
        if (conf > max)
            conf = max;

        return std::min(conf, 17);
    }

    bool AllocPictureBuffer(VkPicIf** pic) final
    {
        fprintf(stdout, "[%lu] %s\n", syscall(SYS_gettid), __FUNCTION__);

        for (auto& apic : m_dpb) {
            if (apic.isAvailable()) {
                apic.AddRef();
                *pic = &apic;
                return true;
            }
        }

        return false;
    }

    bool DecodePicture(VkParserPictureData* pic) final
    {
        fprintf(stdout, "[%lu] %s - %" PRIu32 "\n", syscall(SYS_gettid), __FUNCTION__, pic->nBitstreamDataLen);
        dump_parser_picture_data(codec, pic);
        return true;
    }

    bool UpdatePictureParameters(VkPictureParameters* params, VkSharedBaseObj<VkParserVideoRefCountBase>& shared, uint64_t count) final
    {
        // fprintf(stdout, "%s: %" PRIu64 "\n", __FUNCTION__, count);
        fprintf(stdout, "[%lu] %s\n", syscall(SYS_gettid), __FUNCTION__);
        shared = PictureParameterSet::create();
        dump_picture_parameters(params);
        return true;
    }

    bool DisplayPicture(VkPicIf* pic, int64_t ts) final
    {
        fprintf(stdout, "[%lu] %s\n", syscall(SYS_gettid), __FUNCTION__);
        return true;
    }

    void UnhandledNALU(const uint8_t*, int32_t) final
    {
        fprintf(stdout, "%s\n", __FUNCTION__);
    }

    ~VideoParserClient()
    {
        for (auto& pic : m_dpb)
            assert(pic.isAvailable());
    }

private:
    std::vector<Picture> m_dpb;
};

static bool parse(FILE* stream)
{
    VulkanVideoDecodeParser* parser = nullptr;
    VideoParserClient client = VideoParserClient();
    VkParserInitDecodeParameters params = {
        .interfaceVersion = VK_MAKE_VIDEO_STD_VERSION(0, 9, 1),
        .pClient = &client,
        .bOutOfBandPictureParameters = true,
    };
    bool ret;
    unsigned char buf[BUFSIZ + 1];
    size_t read;
    int32_t parsed;
    VkParserBitstreamPacket pkt;

    fprintf(stdout, "[%lu] %s\n", syscall(SYS_gettid), __FUNCTION__);

    ret = CreateVulkanVideoDecodeParser(&parser, codec, (ParserLogFuncType)printf, 50);
    assert(ret);
    if (!ret)
        return ret;

    ret = (parser->Initialize(&params) == VK_SUCCESS);
    assert(ret);
    if (!ret)
        return ret;

    while (true) {
        read = fread(buf, 1, BUFSIZ, stream);
        if (read <= 0)
            break;
        pkt = (VkParserBitstreamPacket) {
            .pByteStream = buf,
            .nDataLength = static_cast<int32_t>(read),
            .bEOS = read < BUFSIZ,
        };

        if (!parser->ParseByteStream(&pkt, &parsed)) {
            fprintf(stdout, "failed to parse bitstream.\n");
            break;
        }

        assert(pkt.nDataLength == parsed);
    }

    ret = (parser->Release() == 0);
    assert(ret);
    return ret;
}

int main(int argc, char** argv)
{
    int opt;
    FILE* file;

    while ((opt = getopt (argc, argv, "c:")) != -1) {
        switch (opt) {
        case 'c':
            if (strcmp (optarg, "h265") == 0)
                codec = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT;
            break;
        default:
            fprintf (stdout, "%s [-c {h264|h265}] filename\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (optind >= argc) {
        fprintf (stdout, "Missing media file.\n");
        return EXIT_FAILURE;
    }

    file = fopen(argv[optind], "r");
    if (!file) {
        fprintf(stdout, "Unable to open: %s -- %s.\n", argv[1], strerror(errno));
        return EXIT_FAILURE;
    }

    if (!parse(file)) {
        fclose(file);
        return EXIT_FAILURE;
    }

    fclose(file);
    return EXIT_SUCCESS;
}
