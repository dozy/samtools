/*  bam_split.c -- split subcommand.

    Copyright (C) 2013-2015 Genome Research Ltd.

    Author: Martin Pollard <mp15@sanger.ac.uk>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.  */

#include <config.h>

#include <htslib/sam.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>
#include <unistd.h>
#include <regex.h>
#include <htslib/khash.h>
#include <htslib/kstring.h>
#include <htslib/cram.h>
#include "samtools.h"
#include "sam_opts.h"


KHASH_MAP_INIT_STR(c2i, int)

struct parsed_opts {
    char* merged_input_name;
    char* unaccounted_header_name;
    char* unaccounted_name;
    char* output_format_string;
    bool verbose;
    sam_global_args ga;
};

typedef struct parsed_opts parsed_opts_t;

struct state {
    samFile* merged_input_file;
    bam_hdr_t* merged_input_header;
    samFile* unaccounted_file;
    bam_hdr_t* unaccounted_header;
    size_t output_count;
    char** rg_id;
    char **rg_output_file_name;
    samFile** rg_output_file;
    bam_hdr_t** rg_output_header;
    kh_c2i_t* rg_hash;
};

typedef struct state state_t;

static int cleanup_state(state_t* status, bool check_close);
static void cleanup_opts(parsed_opts_t* opts);

static void usage(FILE *write_to)
{
    fprintf(write_to,
"Usage: samtools split [-u <unaccounted.bam>[:<unaccounted_header.sam>]]\n"
"                      [-f <format_string>] [-v] <merged.bam>\n"
"Options:\n"
"  -f STRING       output filename format string [\"%%*_%%#.%%.\"]\n"
"  -u FILE1        put reads with no RG tag or an unrecognised RG tag in FILE1\n"
"  -u FILE1:FILE2  ...and override the header with FILE2\n"
"  -v              verbose output\n");
    sam_global_opt_help(write_to, "-....");
    fprintf(write_to,
"\n"
"Format string expansions:\n"
"  %%%%     %%\n"
"  %%*     basename\n"
"  %%#     @RG index\n"
"  %%!     @RG ID\n"
"  %%.     filename extension for output format\n"
      );
}

// Takes the command line options and turns them into something we can understand
static parsed_opts_t* parse_args(int argc, char** argv)
{
    if (argc == 1) { usage(stdout); return NULL; }

    const char* optstring = "vf:u:";
    char* delim;

    static const struct option lopts[] = {
        SAM_OPT_GLOBAL_OPTIONS('-', 0, 0, 0, 0),
        { NULL, 0, NULL, 0 }
    };

    parsed_opts_t* retval = calloc(sizeof(parsed_opts_t), 1);
    if (! retval ) { perror("cannot allocate option parsing memory"); return NULL; }

    sam_global_args_init(&retval->ga);

    int opt;
    while ((opt = getopt_long(argc, argv, optstring, lopts, NULL)) != -1) {
        switch (opt) {
        case 'f':
            retval->output_format_string = strdup(optarg);
            if (! retval->output_format_string ) { perror("cannot allocate output format string memory"); return NULL; }
            break;
        case 'v':
            retval->verbose = true;
            break;
        case 'u':
            retval->unaccounted_name = strdup(optarg);
            if (! retval->unaccounted_name ) { perror("cannot allocate string memory"); return NULL; }
            if ((delim = strchr(retval->unaccounted_name, ':')) != NULL) {
                *delim = '\0';
                retval->unaccounted_header_name = strdup(delim+1);
                if (! retval->unaccounted_header_name ) { perror("cannot allocate string memory"); return NULL; }
            }
            break;
        default:
            if (parse_sam_global_opt(opt, optarg, lopts, &retval->ga) == 0) break;
            /* else fall-through */
        case '?':
            usage(stdout);
            free(retval);
            return NULL;
        }
    }

    if (retval->output_format_string == NULL) retval->output_format_string = strdup("%*_%#.%.");

    argc -= optind;
    argv += optind;

    if (argc != 1) {
        fprintf(stderr, "Invalid number of arguments: %d\n", argc);
        usage(stderr);
        free(retval);
        return NULL;
    }

    retval->merged_input_name = strdup(argv[0]);
    if (! retval->merged_input_name ) { perror("cannot allocate string memory"); return NULL; }

    return retval;
}

// Expands a output filename format string
static char* expand_format_string(const char* format_string, const char* basename, const char* rg_id, const int rg_idx, const htsFormat *format)
{
    kstring_t str = { 0, 0, NULL };
    const char* pointer = format_string;
    const char* next;
    while ((next = strchr(pointer, '%')) != NULL) {
        kputsn(pointer, next-pointer, &str);
        ++next;
        switch (*next) {
            case '%':
                kputc('%', &str);
                break;
            case '*':
                kputs(basename, &str);
                break;
            case '#':
                kputl(rg_idx, &str);
                break;
            case '!':
                kputs(rg_id, &str);
                break;
            case '.':
                // Only really need to cope with sam, bam, cram
                if (format->format != unknown_format)
                    kputs(hts_format_file_extension(format), &str);
                else
                    kputs("bam", &str);
                break;
            case '\0':
                // Error is: fprintf(stderr, "bad format string, trailing %%\n");
                free(str.s);
                return NULL;
            default:
                // Error is: fprintf(stderr, "bad format string, unknown format specifier\n");
                free(str.s);
                return NULL;
        }
        pointer = next + 1;
    }
    kputs(pointer, &str);
    return ks_release(&str);
}

// Parse the header, count the number of RG tags and return a list of their names
static bool count_RG(bam_hdr_t* hdr, size_t* count, char*** output_name)
{
    if (hdr->l_text < 3 ) {
        *count = 0;
        *output_name = NULL;
        return true;
    }
    kstring_t input = { 0, 0, NULL };
    kputsn(hdr->text, hdr->l_text, &input);

    //////////////////////////////////////////
    // First stage count number of @RG tags //
    //////////////////////////////////////////
    char* pointer = ks_str(&input);
    size_t n_rg = 0;
    // Guard against rare case where @RG is first header line
    // This shouldn't happen but could where @HD is omitted
    if (pointer[0] == '@' && pointer[1] == 'R' && pointer[2] == 'G' ) {
        ++n_rg;
        pointer += 3;
    }
    char* line;
    while ((line = strstr(pointer, "\n@RG")) != NULL) {
        ++n_rg;
        pointer = line + 1;
    }

    //////////////////////////////////
    // Second stage locate @RG ID's //
    //////////////////////////////////
    char** names = (char**)calloc(sizeof(char*), n_rg);
    size_t next = 0;

    regex_t rg_finder;
    if (regcomp(&rg_finder, "^@RG.*\tID:([!-)+-<>-~][ !-~]*)(\t.*$|$)", REG_EXTENDED|REG_NEWLINE) != 0) {
        free(input.s);
        free(names);
        return false;
    }
    regmatch_t* matches = (regmatch_t*)calloc(sizeof(regmatch_t),2);
    int error;
    char* begin = ks_str(&input);

    while ((error = regexec(&rg_finder, begin, 2, matches, 0)) == 0) {
        kstring_t str = { 0, 0, NULL };
        kputsn(begin+matches[1].rm_so, matches[1].rm_eo-matches[1].rm_so, &str);
        names[next++] = ks_release(&str);
        begin += matches[0].rm_eo;
    }

    if (error != REG_NOMATCH) {
        // cleanup
        regfree(&rg_finder);
        free(matches);
        free(names);
        free(input.s);
        return false;
    }
    free(matches);

    // return results
    *count = n_rg;
    *output_name = names;
    regfree(&rg_finder);
    free(input.s);
    return true;
}

// Filters a header of @RG lines where ID != id_keep
// TODO: strip @PG's descended from other RGs and their descendants
static bool filter_header_rg(bam_hdr_t* hdr, const char* id_keep, char *arg_list)
{
    kstring_t str = {0, 0, NULL};

    regex_t rg_finder;

    if (regcomp(&rg_finder, "^@RG.*\tID:([!-)+-<>-~][ !-~]*)(\t.*$|$)", REG_EXTENDED|REG_NEWLINE) != 0) {
        return false;
    }

    // regex vars
    char* header = hdr->text;
    regmatch_t* matches = (regmatch_t*)calloc(sizeof(regmatch_t),2);
    kstring_t found_id = { 0, 0, NULL };
    int error;

    while ((error = regexec(&rg_finder, header, 2, matches, 0)) == 0) {
        kputsn(header, matches[0].rm_so, &str); // copy header up until the found RG line

        found_id.l = 0;
        kputsn(header+matches[1].rm_so, matches[1].rm_eo-matches[1].rm_so, &found_id); // extract ID
        // if it matches keep keep it, else we can just ignore it
        if (strcmp(ks_str(&found_id), id_keep) == 0) {
            kputsn(header+matches[0].rm_so, (matches[0].rm_eo+1)-matches[0].rm_so, &str);
        }
        // move pointer forward
        header += matches[0].rm_eo+1;
    }
    // cleanup
    free(found_id.s);
    free(matches);
    regfree(&rg_finder);
    // Did we leave loop because of an error?
    if (error != REG_NOMATCH) {
        return false;
    }

    // Write remainder of string
    kputs(header, &str);

    // Modify header
    hdr->l_text = ks_len(&str);
    free(hdr->text);
    hdr->text = ks_release(&str);

    // Add the PG line
    SAM_hdr *sh = sam_hdr_parse_(hdr->text, hdr->l_text);
    if (sam_hdr_add_PG(sh, "samtools",
                           "VN", samtools_version(),
                           arg_list ? "CL": NULL,
                           arg_list ? arg_list : NULL,
                           NULL) != 0)
        return -1;

    free(hdr->text);
    hdr->text = strdup(sam_hdr_str(sh));
    hdr->l_text = sam_hdr_length(sh);
    if (!hdr->text)
        return false;
    sam_hdr_free(sh);

    return true;
}

// Set the initial state
static state_t* init(parsed_opts_t* opts, char *arg_list)
{
    state_t* retval = calloc(sizeof(state_t), 1);
    if (!retval) {
        fprintf(stderr, "Out of memory");
        return NULL;
    }

    retval->merged_input_file = sam_open_format(opts->merged_input_name, "rb", &opts->ga.in);
    if (!retval->merged_input_file) {
        fprintf(stderr, "Could not open input file (%s)\n", opts->merged_input_name);
        free(retval);
        return NULL;
    }
    retval->merged_input_header = sam_hdr_read(retval->merged_input_file);
    if (retval->merged_input_header == NULL) {
        fprintf(stderr, "Could not read header for file '%s'\n",
                opts->merged_input_name);
        cleanup_state(retval, false);
        return NULL;
    }

    if (opts->unaccounted_name) {
        if (opts->unaccounted_header_name) {
            samFile* hdr_load = sam_open_format(opts->unaccounted_header_name, "r", &opts->ga.in);
            if (!hdr_load) {
                fprintf(stderr, "Could not open unaccounted header file (%s)\n", opts->unaccounted_header_name);
                cleanup_state(retval, false);
                return NULL;
            }
            retval->unaccounted_header = sam_hdr_read(hdr_load);
            if (retval->unaccounted_header == NULL) {
                fprintf(stderr, "Could not read header for file '%s'\n",
                        opts->unaccounted_header_name);
                cleanup_state(retval, false);
                return NULL;
            }
            sam_close(hdr_load);
        } else {
            retval->unaccounted_header = bam_hdr_dup(retval->merged_input_header);
        }

        retval->unaccounted_file = sam_open_format(opts->unaccounted_name, "wb", &opts->ga.out);
        if (retval->unaccounted_file == NULL) {
            fprintf(stderr, "Could not open unaccounted output file: %s\n", opts->unaccounted_name);
            cleanup_state(retval, false);
            return NULL;
        }
    }

    // Open output files for RGs
    if (!count_RG(retval->merged_input_header, &retval->output_count, &retval->rg_id)) return NULL;
    if (opts->verbose) fprintf(stderr, "@RG's found %zu\n",retval->output_count);

    retval->rg_output_file_name = (char **)calloc(retval->output_count, sizeof(char *));
    retval->rg_output_file = (samFile**)calloc(retval->output_count, sizeof(samFile*));
    retval->rg_output_header = (bam_hdr_t**)calloc(retval->output_count, sizeof(bam_hdr_t*));
    retval->rg_hash = kh_init_c2i();
    if (!retval->rg_output_file_name || !retval->rg_output_file || !retval->rg_output_header || !retval->rg_hash) {
        fprintf(stderr, "Could not allocate memory for output file array. Out of memory?");
        cleanup_state(retval, false);
        return NULL;
    }

    char* dirsep = strrchr(opts->merged_input_name, '/');
    char* input_base_name = strdup(dirsep? dirsep+1 : opts->merged_input_name);
    if (!input_base_name) {
        fprintf(stderr, "Out of memory\n");
        cleanup_state(retval, false);
        return NULL;
    }
    char* extension = strrchr(input_base_name, '.');
    if (extension) *extension = '\0';

    size_t i;
    for (i = 0; i < retval->output_count; i++) {
        char* output_filename = NULL;

        output_filename = expand_format_string(opts->output_format_string,
                                               input_base_name,
                                               retval->rg_id[i], i,
                                               &opts->ga.out);

        if ( output_filename == NULL ) {
            fprintf(stderr, "Error expanding output filename format string.\n");
            cleanup_state(retval, false);
            free(input_base_name);
            return NULL;
        }

        retval->rg_output_file_name[i] = output_filename;
        retval->rg_output_file[i] = sam_open_format(output_filename, "wb", &opts->ga.out);
        if (retval->rg_output_file[i] == NULL) {
            fprintf(stderr, "Could not open output file: %s\n", output_filename);
            cleanup_state(retval, false);
            free(input_base_name);
            return NULL;
        }

        // Record index in hash
        int ret;
        khiter_t iter = kh_put_c2i(retval->rg_hash, retval->rg_id[i], &ret);
        kh_val(retval->rg_hash,iter) = i;

        // Set and edit header
        retval->rg_output_header[i] = bam_hdr_dup(retval->merged_input_header);
        if ( !filter_header_rg(retval->rg_output_header[i], retval->rg_id[i], arg_list) ) {
            fprintf(stderr, "Could not rewrite header for file: %s\r\n", output_filename);
            cleanup_state(retval,false);
            free(output_filename);
            free(input_base_name);
            return NULL;
        }
    }

    free(input_base_name);

    return retval;
}

static bool split(state_t* state, char *arg_list)
{
    if (state->unaccounted_file && sam_hdr_write(state->unaccounted_file, state->unaccounted_header) != 0) {
        fprintf(stderr, "Could not write output file header\n");
        return false;
    }
    size_t i;
    for (i = 0; i < state->output_count; i++) {
        if (sam_hdr_write(state->rg_output_file[i], state->rg_output_header[i]) != 0) {
            fprintf(stderr, "Could not write output file header for '%s'\n",
                    state->rg_output_file_name[i]);
            return false;
        }
    }

    bam1_t* file_read = bam_init1();
    // Read the first record
    int r;
    if ((r=sam_read1(state->merged_input_file, state->merged_input_header, file_read)) < 0) {
        // Nothing more to read?  Ignore this file
        bam_destroy1(file_read);
        file_read = NULL;
        if (r < -1) {
            fprintf(stderr, "Could not read first input record\n");
            return false;
        }
    }

    while (file_read != NULL) {
        // Get RG tag from read and look it up in hash to find file to output it to
        uint8_t* tag = bam_aux_get(file_read, "RG");
        khiter_t iter;
        if ( tag != NULL ) {
            char* rg = bam_aux2Z(tag);
            iter = kh_get_c2i(state->rg_hash, rg);
        } else {
            iter = kh_end(state->rg_hash);
        }

        // Write the read out to correct file
        if (iter != kh_end(state->rg_hash)) {
            // if found write to the appropriate untangled bam
            int i = kh_val(state->rg_hash,iter);
            if (sam_write1(state->rg_output_file[i], state->rg_output_header[i], file_read) < 0) {
                fprintf(stderr, "Could not write to output file '%s'\n",
                        state->rg_output_file_name[i]);
                bam_destroy1(file_read);
                return false;
            }
        } else {
            // otherwise write to the unaccounted bam if there is one or fail
            if (state->unaccounted_file == NULL) {
                if (tag) {
                    fprintf(stderr, "Read \"%s\" with unaccounted for tag \"%s\".\n", bam_get_qname(file_read), bam_aux2Z(tag));
                } else {
                    fprintf(stderr, "Read \"%s\" has no RG tag.\n", bam_get_qname(file_read));
                }
                bam_destroy1(file_read);
                return false;
            } else {
                if (sam_write1(state->unaccounted_file, state->unaccounted_header, file_read) < 0) {
                    fprintf(stderr, "Could not write to unaccounted output file\n");
                    bam_destroy1(file_read);
                    return false;
                }
            }
        }

        // Replace written read with the next one to process
        if ((r=sam_read1(state->merged_input_file, state->merged_input_header, file_read)) < 0) {
            // Nothing more to read?  Ignore this file in future
            bam_destroy1(file_read);
            file_read = NULL;
            if (r < -1) {
                fprintf(stderr, "Could not read input record\n");
                return false;
            }
        }
    }

    return true;
}

static int cleanup_state(state_t* status, bool check_close)
{
    int ret = 0;

    if (!status) return 0;
    if (status->unaccounted_header) bam_hdr_destroy(status->unaccounted_header);
    if (status->unaccounted_file) {
        if (sam_close(status->unaccounted_file) < 0 && check_close) {
            fprintf(stderr, "Error on closing unaccounted file\n");
            ret = -1;
        }
    }
    sam_close(status->merged_input_file);
    size_t i;
    for (i = 0; i < status->output_count; i++) {
        if (status->rg_output_header && status->rg_output_header[i])
            bam_hdr_destroy(status->rg_output_header[i]);
        if (status->rg_output_file && status->rg_output_file[i]) {
            if (sam_close(status->rg_output_file[i]) < 0 && check_close) {
                fprintf(stderr, "Error on closing output file '%s'\n",
                        status->rg_output_file_name[i]);
                ret = -1;
            }
        }
        if (status->rg_id) free(status->rg_id[i]);
        if (status->rg_output_file_name) free(status->rg_output_file_name[i]);
    }
    if (status->merged_input_header)
        bam_hdr_destroy(status->merged_input_header);
    free(status->rg_output_header);
    free(status->rg_output_file);
    free(status->rg_output_file_name);
    kh_destroy_c2i(status->rg_hash);
    free(status->rg_id);
    free(status);

    return ret;
}

static void cleanup_opts(parsed_opts_t* opts)
{
    if (!opts) return;
    free(opts->merged_input_name);
    free(opts->unaccounted_header_name);
    free(opts->unaccounted_name);
    free(opts->output_format_string);
    sam_global_args_free(&opts->ga);
    free(opts);
}

int main_split(int argc, char** argv)
{
    int ret = 1;
    char *arg_list = stringify_argv(argc+1, argv-1);
    parsed_opts_t* opts = parse_args(argc, argv);
    if (!opts) goto cleanup_opts;
    state_t* status = init(opts,arg_list);
    if (!status) goto cleanup_opts;

    if (!split(status,arg_list)) {
        cleanup_state(status, false);
        goto cleanup_opts;
    }

    ret = cleanup_state(status, true);

cleanup_opts:
    cleanup_opts(opts);

    return ret;
}
