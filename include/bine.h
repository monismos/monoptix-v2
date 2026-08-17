/*
 * bine.h - Public API for the BINE binary superoptimizer
 *
 * The pure-binary programming language.  Programs are .bbf files
 * (binary interaction nets).  The pipeline is:
 *   .bbf -> parse -> optimize (Kolmogorov descent) -> execute (GPU)
 *
 * Modes:
 *   bine run <file.bbf>          parse, optimize, execute
 *   bine optimize <file.bbf>     parse, optimize, write <file.bbf>.opt
 *   bine compile <in.bbf> <out.elf>  parse, optimize, emit ELF
 *   bine bench <file.bbf>        run with timing
 *   bine verify <file.bbf>       differential test against reference
 *   bine info <file.bbf>         show net summary
 *   bine from-elf <in.elf> <out.bbf>  ELF -> .bbf (DFG conversion)
 *   bine elf-optimize <in.elf> <out.elf>  ELF -> ELF (DFG + optimize)
 *   bine diag                    print Diagonal Logic properties
 *   bine help                    show usage
 */

#ifndef BINE_H
#define BINE_H

/* Master umbrella header.  Pulls in the internal ELF-pipeline API
 * (BinaryIntrinsic, Instruction, StrandMasks, bine_optimize, ...) as well
 * as the pure-binary (.bbf) pipeline API. */
#include "bine_compat.h"
#include "bine_internal.h"

#include "bbf.h"
#include "kolmogorov.h"
#include "diagonal.h"
#include "zentropy.h"
#include "dfg.h"
#include "gpu_runner.h"
#include "cpu_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

/* Top-level pipeline: parse, optimize, (optionally) execute. */
typedef enum {
    BINE_PIPE_PARSE      = 0x01,
    BINE_PIPE_OPTIMIZE   = 0x02,
    BINE_PIPE_EXECUTE    = 0x04,
    BINE_PIPE_SERIALIZE  = 0x08,
    BINE_PIPE_DFG_BUILD  = 0x10,
} bine_pipeline_stage_t;

#define BINE_PIPE_FULL (BINE_PIPE_PARSE | BINE_PIPE_OPTIMIZE | \
                        BINE_PIPE_SERIALIZE)
#define BINE_PIPE_RUN  (BINE_PIPE_PARSE | BINE_PIPE_OPTIMIZE | \
                        BINE_PIPE_EXECUTE)

/* Run the full pipeline.  Returns 0 on success. */
int bine_pipeline_run(const char *input_path,
                      const char *output_path,
                      int stages,
                      bine_net_t *out_net);

/* CLI: parse argv and dispatch.  Returns process exit code. */
int bine_cli(int argc, char **argv);

/* Version string. */
const char *bine_version(void);

#ifdef __cplusplus
}
#endif

#endif /* BINE_H */
