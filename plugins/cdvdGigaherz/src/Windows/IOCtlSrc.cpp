/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2016  PCSX2 Dev Team
 *  Copyright (C) 2002-2014 David Quintana [gigaherz]
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "../CDVD.h"

#include <winioctl.h>
#include <ntddcdvd.h>
#include <ntddcdrm.h>
// "typedef ignored" warning will disappear once we move to the Windows 10 SDK.
#pragma warning(push)
#pragma warning(disable : 4091)
#include <ntddscsi.h>
#pragma warning(pop)

#include <cstddef>
#include <cstdlib>
#include <array>

IOCtlSrc::IOCtlSrc(const char *filename)
    : m_filename(filename)
{
    Reopen();
    SetSpindleSpeed(false);
}

IOCtlSrc::~IOCtlSrc()
{
    if (OpenOK) {
        SetSpindleSpeed(true);
        CloseHandle(m_device);
    }
}

s32 IOCtlSrc::Reopen()
{
    if (m_device != INVALID_HANDLE_VALUE)
        CloseHandle(m_device);

    DWORD size;

    OpenOK = false;
    // SPTI only works if the device is opened with GENERIC_WRITE access.
    m_device = CreateFile(m_filename.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (m_device == INVALID_HANDLE_VALUE)
        return -1;

    // Required to read from layer 1 of Dual layer DVDs
    DeviceIoControl(m_device, FSCTL_ALLOW_EXTENDED_DASD_IO, nullptr, 0, nullptr, 0, &size, nullptr);

    m_disc_ready = false;
    OpenOK = true;
    return 0;
}

void IOCtlSrc::SetSpindleSpeed(bool restore_defaults)
{

    DWORD dontcare;
    USHORT speed = 0;

    if (GetMediaType() < 0)
        speed = 4800; // CD-ROM to ~32x (PS2 has 24x (3600 KB/s))
    else
        speed = 11080; // DVD-ROM to  ~8x (PS2 has 4x (5540 KB/s))

    if (!restore_defaults) {
        CDROM_SET_SPEED s;
        s.RequestType = CdromSetSpeed;
        s.RotationControl = CdromDefaultRotation;
        s.ReadSpeed = speed;
        s.WriteSpeed = speed;

        if (DeviceIoControl(m_device,
                            IOCTL_CDROM_SET_SPEED, //operation to perform
                            &s, sizeof(s),         //no input buffer
                            NULL, 0,               //output buffer
                            &dontcare,             //#bytes returned
                            (LPOVERLAPPED)NULL))   //synchronous I/O == 0)
        {
            printf(" * CDVD: setSpindleSpeed success (%uKB/s)\n", speed);
        } else {
            printf(" * CDVD: setSpindleSpeed failed! \n");
        }
    } else {
        CDROM_SET_SPEED s;
        s.RequestType = CdromSetSpeed;
        s.RotationControl = CdromDefaultRotation;
        s.ReadSpeed = 0xffff; // maximum ?
        s.WriteSpeed = 0xffff;

        DeviceIoControl(m_device,
                        IOCTL_CDROM_SET_SPEED, //operation to perform
                        &s, sizeof(s),         //no input buffer
                        NULL, 0,               //output buffer
                        &dontcare,             //#bytes returned
                        (LPOVERLAPPED)NULL);   //synchronous I/O == 0)
    }
}

u32 IOCtlSrc::GetSectorCount()
{
    if (!m_disc_ready)
        RefreshDiscInfo();

    return m_sectors;
}

u32 IOCtlSrc::GetLayerBreakAddress()
{
    if (!m_disc_ready)
        RefreshDiscInfo();

    if (GetMediaType() < 0)
        return 0;

    return m_layer_break;
}

s32 IOCtlSrc::GetMediaType()
{
    if (!m_disc_ready)
        RefreshDiscInfo();

    return m_media_type;
}

const std::vector<toc_entry> &IOCtlSrc::ReadTOC()
{
    if (!m_disc_ready)
        RefreshDiscInfo();

    return m_toc;
}

s32 IOCtlSrc::ReadSectors2048(u32 sector, u32 count, char *buffer)
{
    RAW_READ_INFO rri;

    DWORD size = 0;

    if (!OpenOK)
        return -1;

    rri.DiskOffset.QuadPart = sector * (u64)2048;
    rri.SectorCount = count;

    //fall back to standard reading
    if (SetFilePointer(m_device, rri.DiskOffset.LowPart, &rri.DiskOffset.HighPart, FILE_BEGIN) == -1) {
        if (GetLastError() != 0)
            return -1;
    }

    if (ReadFile(m_device, buffer, 2048 * count, &size, NULL) == 0) {
        return -1;
    }

    if (size != (2048 * count)) {
        return -1;
    }

    return 0;
}


s32 IOCtlSrc::ReadSectors2352(u32 sector, u32 count, char *buffer)
{
    if (!OpenOK)
        return -1;

    struct sptdinfo
    {
        SCSI_PASS_THROUGH_DIRECT info;
        char sense_buffer[20];
    } sptd{};

    // READ CD command
    sptd.info.Cdb[0] = 0xBE;
    // Don't care about sector type.
    sptd.info.Cdb[1] = 0;
    // Number of sectors to read
    sptd.info.Cdb[6] = 0;
    sptd.info.Cdb[7] = 0;
    sptd.info.Cdb[8] = 1;
    // Sync + all headers + user data + EDC/ECC. Excludes C2 + subchannel
    sptd.info.Cdb[9] = 0xF8;
    sptd.info.Cdb[10] = 0;
    sptd.info.Cdb[11] = 0;

    sptd.info.CdbLength = 12;
    sptd.info.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    sptd.info.DataIn = SCSI_IOCTL_DATA_IN;
    sptd.info.SenseInfoOffset = offsetof(sptdinfo, sense_buffer);
    sptd.info.TimeOutValue = 5;

    // Read sectors one by one to avoid reading data from 2 tracks of different
    // types in the same read (which will fail).
    for (u32 n = 0; n < count; ++n) {
        u32 current_sector = sector + n;
        sptd.info.Cdb[2] = (current_sector >> 24) & 0xFF;
        sptd.info.Cdb[3] = (current_sector >> 16) & 0xFF;
        sptd.info.Cdb[4] = (current_sector >> 8) & 0xFF;
        sptd.info.Cdb[5] = current_sector & 0xFF;
        sptd.info.DataTransferLength = 2352;
        sptd.info.DataBuffer = buffer + 2352 * n;
        sptd.info.SenseInfoLength = sizeof(sptd.sense_buffer);

        DWORD unused;
        if (DeviceIoControl(m_device, IOCTL_SCSI_PASS_THROUGH_DIRECT, &sptd,
                            sizeof(sptd), &sptd, sizeof(sptd), &unused, nullptr)) {
            if (sptd.info.DataTransferLength != 2352)
                printf(" * CDVD: SPTI short transfer of %u bytes", sptd.info.DataTransferLength);
            continue;
        }
        printf(" * CDVD: SPTI failed reading sector %u; SENSE %u -", current_sector, sptd.info.SenseInfoLength);
        for (const auto &c : sptd.sense_buffer)
            printf(" %02X", c);
        putchar('\n');
        return -1;
    }

    return 0;
}

bool IOCtlSrc::ReadDVDInfo()
{
    DWORD unused;
    DVD_SESSION_ID session_id;

    BOOL ret = DeviceIoControl(m_device, IOCTL_DVD_START_SESSION, nullptr, 0,
                               &session_id, sizeof(session_id), &unused, nullptr);
    if (!ret)
        return false;

    // 4 bytes header + 18 bytes layer descriptor - Technically you only need
    // to read 17 bytes of the layer descriptor since bytes 17-2047 is for
    // media specific information. However, Windows requires you to read at
    // least 18 bytes of the layer descriptor or else the ioctl will fail. The
    // media specific information seems to be empty, so there's no point reading
    // any more than that.
    std::array<u8, 22> buffer;
    DVD_READ_STRUCTURE dvdrs{{0}, DvdPhysicalDescriptor, session_id, 0};

    ret = DeviceIoControl(m_device, IOCTL_DVD_READ_STRUCTURE, &dvdrs, sizeof(dvdrs),
                          buffer.data(), buffer.size(), &unused, nullptr);
    if (ret) {
        auto &layer = *reinterpret_cast<DVD_LAYER_DESCRIPTOR *>(
            reinterpret_cast<DVD_DESCRIPTOR_HEADER *>(buffer.data())->Data);

        u32 start_sector = _byteswap_ulong(layer.StartingDataSector);
        u32 end_sector = _byteswap_ulong(layer.EndDataSector);

        if (layer.NumberOfLayers == 0) {
            // Single layer
            m_media_type = 0;
            m_layer_break = 0;
            m_sectors = end_sector - start_sector + 1;
        } else if (layer.TrackPath == 0) {
            // Dual layer, Parallel Track Path
            dvdrs.LayerNumber = 1;
            ret = DeviceIoControl(m_device, IOCTL_DVD_READ_STRUCTURE, &dvdrs,
                                  sizeof(dvdrs), buffer.data(), buffer.size(), &unused, nullptr);
            if (ret) {
                u32 layer1_start_sector = _byteswap_ulong(layer.StartingDataSector);
                u32 layer1_end_sector = _byteswap_ulong(layer.EndDataSector);

                m_media_type = 1;
                m_layer_break = end_sector - start_sector;
                m_sectors = end_sector - start_sector + 1 + layer1_end_sector - layer1_start_sector + 1;
            }
        } else {
            // Dual layer, Opposite Track Path
            u32 end_sector_layer0 = _byteswap_ulong(layer.EndLayerZeroSector);
            m_media_type = 2;
            m_layer_break = end_sector_layer0 - start_sector;
            m_sectors = end_sector_layer0 - start_sector + 1 + end_sector - (~end_sector_layer0 & 0xFFFFFFU) + 1;
        }
    }

    DeviceIoControl(m_device, IOCTL_DVD_END_SESSION, &session_id,
                    sizeof(session_id), nullptr, 0, &unused, nullptr);

    return !!ret;
}

bool IOCtlSrc::ReadCDInfo()
{
    DWORD unused;
    CDROM_READ_TOC_EX toc_ex{};
    toc_ex.Format = CDROM_READ_TOC_EX_FORMAT_TOC;
    toc_ex.Msf = 0;
    toc_ex.SessionTrack = 1;

    CDROM_TOC toc;
    if (!DeviceIoControl(m_device, IOCTL_CDROM_READ_TOC_EX, &toc_ex,
                         sizeof(toc_ex), &toc, sizeof(toc), &unused, nullptr))
        return false;

    m_toc.clear();
    size_t track_count = ((toc.Length[0] << 8) + toc.Length[1] - 2) / sizeof(TRACK_DATA);
    for (size_t n = 0; n < track_count; ++n) {
        TRACK_DATA &track = toc.TrackData[n];
        // Exclude the lead-out track descriptor.
        if (track.TrackNumber == 0xAA)
            continue;
        u32 lba = (track.Address[1] << 16) + (track.Address[2] << 8) + track.Address[3];
        m_toc.push_back({lba, track.TrackNumber, track.Adr, track.Control});
    }

    GET_LENGTH_INFORMATION info;
    if (!DeviceIoControl(m_device, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &info,
                         sizeof(info), &unused, nullptr))
        return false;

    m_sectors = static_cast<u32>(info.Length.QuadPart / 2048);
    m_media_type = -1;

    return true;
}

bool IOCtlSrc::RefreshDiscInfo()
{
    if (m_disc_ready)
        return true;

    m_media_type = 0;
    m_layer_break = 0;
    m_sectors = 0;

    if (!OpenOK)
        return false;

    if (ReadDVDInfo() || ReadCDInfo())
        m_disc_ready = true;

    return m_disc_ready;
}

s32 IOCtlSrc::DiscChanged()
{
    DWORD size = 0;

    if (!OpenOK)
        return -1;

    int ret = DeviceIoControl(m_device, IOCTL_STORAGE_CHECK_VERIFY, NULL, 0, NULL, 0, &size, NULL);

    if (ret == 0) {
        m_disc_ready = false;

        return 1;
    }

    return 0;
}

s32 IOCtlSrc::IsOK()
{
    return OpenOK;
}
