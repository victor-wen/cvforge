/*
 * CVF-001 independent black-box test - brief B9: header self-containment.
 *
 * <cvforwin/cvf_api.h> must compile as strict C11 using only <stdint.h> and
 * <stddef.h>. This translation unit only includes the public header; it must be
 * compiled with the standard include search path disabled and the stub headers
 * from tests/abi/isolation_stub/ used instead, so that any dependency on a C++
 * library, OpenCV, vendor, or Windows header is a compile error.
 *
 * RED / verify command:
 *   gcc -std=c11 -Wall -Wextra -Werror -nostdinc \
 *       -I tests/abi/isolation_stub -I include \
 *       -fsyntax-only tests/abi/test_header_isolation.c
 *
 * Authored by test-engineer from .ai/test-briefs/CVF-001.yaml and the approved
 * .ai/project-contract.yaml only; no production implementation was consulted.
 */
#include <cvforwin/cvf_api.h>

int main(void)
{
    return 0;
}
