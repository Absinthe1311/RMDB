#include <iostream>
#include "../src/index/ix_defs.h"
#include "../src/common/rid.h"

int main() {
    std::cout << "sizeof(IxPageHdr) = " << sizeof(IxPageHdr) << std::endl;
    std::cout << "sizeof(Rid) = " << sizeof(Rid) << std::endl;
    std::cout << "PAGE_SIZE = " << PAGE_SIZE << std::endl;
    
    int col_tot_len = 4;  // int类型
    int available = PAGE_SIZE - sizeof(IxPageHdr);
    int btree_order = available / (col_tot_len + sizeof(Rid)) - 1;
    
    std::cout << "\n容量计算：" << std::endl;
    std::cout << "btree_order = " << btree_order << std::endl;
    std::cout << "get_max_size() = " << (btree_order + 1) << std::endl;
    std::cout << "keys_size = " << ((btree_order + 1) * col_tot_len) << std::endl;
    
    return 0;
}
