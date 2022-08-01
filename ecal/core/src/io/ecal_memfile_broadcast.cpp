/* ========================= eCAL LICENSE =================================
 *
 * Copyright (C) 2016 - 2019 Continental Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * ========================= eCAL LICENSE =================================
*/

/**
 * @brief  eCAL memory file broadcast interface
**/

#include "ecal_memfile_broadcast.h"

#include "ecal_memfile.h"

#include <iostream>

namespace eCAL
{
  PADDING_DISABLED(struct SMemfileBroadcastHeaderV1
  {
    std::uint8_t magic[5] = "eCAL";
    std::uint8_t __reserved_field;
    std::uint32_t version = 1;
    std::uint64_t message_queue_offset = sizeof(SMemfileBroadcastHeaderV1);
    TimestampT timestamp = CreateTimestamp();
  });

  static SMemfileBroadcastHeaderV1 *GetMemfileHeader(void *address)
  {
    return reinterpret_cast<SMemfileBroadcastHeaderV1 *>(address);
  }

  static const SMemfileBroadcastHeaderV1 *GetMemfileHeader(const void *address)
  {
    return reinterpret_cast<const SMemfileBroadcastHeaderV1 *>(address);
  }

  static void *GetMessageQueueAddress(void *address)
  {
    return reinterpret_cast<void *>(static_cast<char *>(address) + GetMemfileHeader(address)->message_queue_offset);
  }

  CMemoryFileBroadcast::CMemoryFileBroadcast(): m_broadcast_memfile(std::make_unique<eCAL::CMemoryFile>()) 
  {
  }

  bool CMemoryFileBroadcast::Create(const std::string &name, std::size_t max_queue_size)
  {
    if (m_created) return false;

    m_max_queue_size = max_queue_size;
    m_name = name;
    const auto presumably_memfile_size =
      RelocatableCircularQueue<SMemfileBroadcastMessage>::PresumablyOccupiedMemorySize(m_max_queue_size) +
      sizeof(SMemfileBroadcastHeaderV1);
    if (!m_broadcast_memfile->Create(name.c_str(), true, presumably_memfile_size))
    {
      if (m_broadcast_memfile->Create(name.c_str(), false, 0))
      {
        
      } else {
        std::cerr << "Unable to read broadcast memory file." << std::endl;
        return false;
      }
    }

    if (m_broadcast_memfile->MaxDataSize() < presumably_memfile_size)
    {
      std::cerr << "Invalid broadcast memory file size." << std::endl;
      return false;
    }

    m_broadcast_memfile_local_buffer.resize(presumably_memfile_size);

    if (m_broadcast_memfile->GetWriteAccess(200))
    {
      // Is acquired lock consistent?
      void *memfile_address = nullptr;
      m_broadcast_memfile->GetWriteAddress(memfile_address, m_broadcast_memfile->MaxDataSize());
      if (!IsMemfileInitialized(memfile_address))
        ResetMemfile(memfile_address);
      if (!IsMemfileVersionCompatible(memfile_address))
      {
        std::cerr << "Broadcast memory file is not compatible" << std::endl;
        return false;
      }

      m_broadcast_memfile->ReleaseWriteAccess();
    }
    else
      return false;

    m_created = true;
    return true;
  }

  bool CMemoryFileBroadcast::Destroy()
  {
    if (!m_created) return false;
    m_broadcast_memfile->Destroy(false);
    m_created = false;
    return true;
  }

  std::string CMemoryFileBroadcast::GetName() const
  {
    return m_name;
  }

  bool CMemoryFileBroadcast::IsMemfileInitialized(const void *memfile_address) const
  {
    const auto *header = GetMemfileHeader(memfile_address);
    return (std::memcmp(header->magic, SMemfileBroadcastHeaderV1().magic, sizeof(SMemfileBroadcastHeaderV1::magic)) ==
            0);
  }

  bool CMemoryFileBroadcast::IsMemfileVersionCompatible(const void *memfile_address) const
  {
    const auto *header = GetMemfileHeader(memfile_address);
    return (header->version == SMemfileBroadcastHeaderV1().version);
  }

  void CMemoryFileBroadcast::ResetMemfile(void *memfile_address)
  {
    auto *header = GetMemfileHeader(memfile_address);
    *header = SMemfileBroadcastHeaderV1();
    m_message_queue.SetBaseAddress(GetMessageQueueAddress(memfile_address));
    m_message_queue.Reset(m_max_queue_size);
  }

  bool CMemoryFileBroadcast::FlushLocalBroadcastQueue()
  {
    if (!m_created) return false;

    if (m_broadcast_memfile->GetReadAccess(200))
    {
      // Is acquired lock consistent?

      const void *memfile_address = nullptr;
      m_broadcast_memfile->GetReadAddress(memfile_address, m_broadcast_memfile->MaxDataSize());
      m_last_timestamp = GetMemfileHeader(memfile_address)->timestamp;
      m_broadcast_memfile->ReleaseReadAccess();
    }
    else
      return false;

    return true;
  }

  bool CMemoryFileBroadcast::FlushGlobalBroadcastQueue()
  {
    if (!m_created) return false;

    if (m_broadcast_memfile->GetWriteAccess(200))
    {
      // Is acquired lock consistent?
      void *memfile_address = nullptr;
      m_broadcast_memfile->GetWriteAddress(memfile_address, m_broadcast_memfile->MaxDataSize());
      ResetMemfile(memfile_address);
      m_broadcast_memfile->ReleaseReadAccess();
    }
    else
      return false;

    return true;
  }

  bool CMemoryFileBroadcast::Broadcast(UniqueIdT payload_memfile_id, eMemfileBroadcastMessageType type)
  {
    if (!m_created) return false;

    if (m_broadcast_memfile->GetWriteAccess(200))
    {
      void *memfile_address = nullptr;
      m_broadcast_memfile->GetWriteAddress(memfile_address, m_broadcast_memfile->MaxDataSize());
      // Is acquired lock consistent?

      m_message_queue.SetBaseAddress(GetMessageQueueAddress(memfile_address));
      const auto timestamp = CreateTimestamp();
      m_message_queue.Push({g_process_id, timestamp, payload_memfile_id, type});
      GetMemfileHeader(memfile_address)->timestamp = timestamp;

      m_broadcast_memfile->ReleaseWriteAccess();
      return true;
    }
    else
    {
      std::cerr << "Unable to acquire write access on broadcast memory file" << std::endl;
      return false;
    }
  }


  bool CMemoryFileBroadcast::ReceiveBroadcast(MemfileBroadcastMessageListT &message_list, TimestampT timeout, bool enable_loopback)
  {
    if (m_broadcast_memfile->GetReadAccess(200))
    {
      // Is acquired lock consistent?

      m_broadcast_memfile->Read(m_broadcast_memfile_local_buffer.data(), m_broadcast_memfile_local_buffer.size(), 0);
      m_broadcast_memfile->ReleaseReadAccess();
    }
    else
    {
      std::cerr << "Unable to acquire read access on broadcast memory file" << std::endl;
      return false;
    }

    message_list.clear();

    const auto *header = GetMemfileHeader(m_broadcast_memfile_local_buffer.data());
    m_message_queue.SetBaseAddress(GetMessageQueueAddress(m_broadcast_memfile_local_buffer.data()));

    const auto timeout_threshold = CreateTimestamp() - (timeout * 1000);
    for (const auto &broadcast_message: m_message_queue)
    {
      const auto timestamp = broadcast_message.timestamp;
      if ((timeout && (timestamp <= timeout_threshold)) || (timestamp <= m_last_timestamp))
        break;
      if ((broadcast_message.process_id == g_process_id) && !enable_loopback)
        continue;
      message_list.push_back(&broadcast_message);
    }

    m_last_timestamp = header->timestamp;

    return true;
  }
}
