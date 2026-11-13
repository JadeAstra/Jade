// Included last, after its other includes, in each file of the compiler side (the compiler,
// checker, parser, lexer, the cache and the ww command): the functions the file defines
// after it go in a section of the executable of their own, which `ww build` leaves out of a
// built program (M6.5 part 5, D161). Only the file's own code goes there: the inline functions
// and templates of the headers above stay in the common code, which the build checks
// (ww --check-sections). Nothing here may run as the program starts: each global of such a
// file is made on first use. No include guard: each file that includes it is to have it.

#if defined(_MSC_VER)
#pragma code_seg(".wwc")
#endif
