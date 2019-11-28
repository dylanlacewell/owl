// ======================================================================== //
// Copyright 2019 Ingo Wald                                                 //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#pragma once

#include "ll/cuda.h"
#include "abstract/common.h"

namespace owl {
  namespace ll {
    
    struct DeviceMemory {
      inline bool   empty() const { return !valid(); }
      inline bool   valid() const { return d_pointer != 0 && sizeInBytes > 0; }
      inline size_t size()  const { return sizeInBytes; }
    
      inline void alloc(size_t size);
      inline void *get();
      inline void upload(const void *h_pointer, const char *debugMessage = nullptr);
      inline void download(void *h_pointer);
      inline void free();
      template<typename T>
      inline void upload(const std::vector<T> &vec);
      
      size_t      sizeInBytes { 0 };
      CUdeviceptr d_pointer   { 0 };
    };
    
    inline void DeviceMemory::alloc(size_t size)
    {
      assert(empty());
      this->sizeInBytes = size;
      CUDA_CHECK(cudaMalloc( (void**)&d_pointer, sizeInBytes));
      assert(valid());
    }
    
    inline void *DeviceMemory::get()
    {
      assert(valid());
      return (void*)d_pointer;
    }

    inline void DeviceMemory::upload(const void *h_pointer, const char *debugMessage)
    {
      assert(valid());
      CUDA_CHECK2(debugMessage,
                  cudaMemcpy((void*)d_pointer, h_pointer,
                             sizeInBytes, cudaMemcpyHostToDevice));
    }
    
    inline void DeviceMemory::download(void *h_pointer)
    {
      assert(valid());
      CUDA_CHECK(cudaMemcpy(h_pointer, (void*)d_pointer, 
                            sizeInBytes, cudaMemcpyDeviceToHost));
    }
    
    inline void DeviceMemory::free()
    {
      assert(valid());
      CUDA_CHECK(cudaFree((void*)d_pointer));
      sizeInBytes = 0;
      d_pointer   = 0;
      assert(empty());
    }

    template<typename T>
    inline void DeviceMemory::upload(const std::vector<T> &vec)
    {
      if (!valid()) {
        alloc(vec.size()*sizeof(T));
      } else {
        assert(size() == vec.size()*sizeof(T));
      }
      assert(valid());
      upload(vec.data());
    }
    
    
  } // ::owl::ll
} //::owl