#include <iostream>
#include <cassert>
#include "parser.h"

int main() {
    std::vector<std::string> sqls = {
        "SELECT company, order_number FROM orders ORDER BY order_number;",
        "SELECT company, order_number FROM orders ORDER BY company, order_number;",
        "SELECT company, order_number FROM orders ORDER BY company DESC, order_number ASC;",
        "SELECT company, order_number FROM orders ORDER BY order_number ASC LIMIT 2;",
        "SELECT company, order_number FROM orders LIMIT 2;",
        "SELECT * FROM orders ORDER BY company DESC LIMIT 3;",
    };
    
    std::cout << "Testing ORDER BY and LIMIT syntax..." << std::endl;
    
    for (auto &sql : sqls) {
        std::cout << "\nSQL: " << sql << std::endl;
        YY_BUFFER_STATE buf = yy_scan_string(sql.c_str());
        if (yyparse() == 0) {
            if (ast::parse_tree != nullptr) {
                ast::TreePrinter::print(ast::parse_tree);
                yy_delete_buffer(buf);
                std::cout << "✓ Parse successful" << std::endl;
            } else {
                std::cout << "✗ Parse tree is null" << std::endl;
            }
        } else {
            std::cout << "✗ Parse failed" << std::endl;
            yy_delete_buffer(buf);
        }
    }
    
    ast::parse_tree.reset();
    return 0;
}
