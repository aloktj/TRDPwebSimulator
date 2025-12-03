#include <trdp/api/trdp_if_light.h>

#include <array>
#include <iostream>

int main()
{
    constexpr std::size_t kHeapSize = 16 * 1024;
    std::array<UINT8, kHeapSize> heap{};

    TRDP_MEM_CONFIG_T memConfig{};
    memConfig.p = heap.data();
    memConfig.size = static_cast<UINT32>(heap.size());

    TRDP_ERR_T initErr = tlc_init(nullptr, nullptr, &memConfig);
    if (initErr != TRDP_NO_ERR)
    {
        std::cerr << "TRDP library detected but initialization failed: error " << initErr << std::endl;
        return 1;
    }

    tlc_terminate();
    std::cout << "TRDP library detected and initialized successfully." << std::endl;
    return 0;
}
