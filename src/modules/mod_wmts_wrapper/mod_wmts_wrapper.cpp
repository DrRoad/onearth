/*
* Copyright (c) 2002-2017, California Institute of Technology.
* All rights reserved.  Based on Government Sponsored Research under contracts NAS7-1407 and/or NAS7-03001.
*
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
*   1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
*   2. Redistributions in binary form must reproduce the above copyright notice,
*      this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
*   3. Neither the name of the California Institute of Technology (Caltech), its operating division the Jet Propulsion Laboratory (JPL),
*      the National Aeronautics and Space Administration (NASA), nor the names of its contributors may be used to
*      endorse or promote products derived from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
* INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
* IN NO EVENT SHALL THE CALIFORNIA INSTITUTE OF TECHNOLOGY BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
* EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
* STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
* EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include <apr.h>
#include <apr_general.h>
#include <httpd.h>
#include <http_config.h>
#include <http_log.h>
#include <http_protocol.h>
#include <http_request.h>
#include <apr_tables.h>
#include <apr_strings.h>
#include <apr_lib.h>

#include "mod_wmts_wrapper.h"
#include "mod_reproject.h"
#include "mod_mrf.h"
#include "receive_context.h"
#include <jansson.h>


// If the condition is met, sends the message to the error log and returns HTTP INTERNAL ERROR
#define SERR_IF(X, msg) if (X) { \
    ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server, msg);\
    return HTTP_INTERNAL_SERVER_ERROR; \
}

// Check a file extension against the specified MIME type
int check_valid_extension(wmts_wrapper_conf *dconf, const char *extension)
{
    wmts_wrapper_conf *cfg = (wmts_wrapper_conf *)dconf;
    if (apr_strnatcasecmp(cfg->mime_type, "image/png") == 0) {
        return (apr_strnatcasecmp(".png", extension) == 0);
    }

    if (apr_strnatcasecmp(cfg->mime_type, "image/jpeg") == 0) {
        return (apr_strnatcasecmp(".jpg", extension) == 0 || apr_strnatcasecmp(".jpeg", extension) == 0);
    }

    // Vector tiles are all the same despite differing extensions and MIME types
    if (apr_strnatcasecmp(cfg->mime_type, "application/vnd.mapbox-vector-tile") == 0
            || apr_strnatcasecmp(cfg->mime_type, "application/x-protobuf;type=mapbox-vector") == 0) {
        return (apr_strnatcasecmp(".pbf", extension) == 0 || apr_strnatcasecmp(".mvt", extension) == 0);
    }

    if (apr_strnatcasecmp(cfg->mime_type, "image/tiff") == 0) {
        return (apr_strnatcasecmp(".tif", extension) == 0 || apr_strnatcasecmp(".tiff", extension) == 0);
    }

    if (apr_strnatcasecmp(cfg->mime_type, "image/lerc") == 0) {
        return (apr_strnatcasecmp(".lerc", extension) == 0);
    }
    return NULL;
}

// argstr_to_table and argstr_to_table are taken from Apache 2.4
void argstr_to_table(char *str, apr_table_t *parms)
{
    char *key;
    char *value;
    char *strtok_state;

    if (str == NULL) {
        return;
    }

    key = apr_strtok(str, "&", &strtok_state);
    // int i;
    // for (i=0;key[i]!=0;i++) key[i]=apr_toupper(key[i]);
    while (key) {
        value = strchr(key, '=');
        if (value) {
            *value = '\0';      /* Split the string in two */
            value++;            /* Skip passed the = */
        }
        else {
            value = NULL;
        }
        ap_unescape_url(key);
        ap_unescape_url(value);
        apr_table_set(parms, key, value);
        key = apr_strtok(NULL, "&", &strtok_state);
    }
}

void ap_args_to_table(request_rec *r, apr_table_t **table)
{
    apr_table_t *t = apr_table_make(r->pool, 10);
    argstr_to_table(apr_pstrdup(r->pool, r->args), t);
    *table = t;
}

static const char *get_base_uri(request_rec *r)
{
    const char *uri = r->uri;
    int uri_len = strlen(uri);
    int i;
    for (i=0;i<uri_len; i++)
    {
        if (uri[uri_len-i] == '/') break;
    }
    return apr_pstrmemdup(r->pool, uri, uri_len-i);
}

static wmts_error wmts_make_error(int status, const char *exceptionCode, const char *locator, const char *exceptionText)
{

    wmts_error error;
    error.status = status;
    error.exceptionCode = exceptionCode;
    error.locator = locator;
    error.exceptionText = exceptionText;

    return error;
}

static int wmts_return_all_errors(request_rec *r, int errors, wmts_error *wmts_errors)
{

    static char preamble[]=
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<ExceptionReport xmlns=\"http://www.opengis.net/ows/1.1\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:schemaLocation=\"http://schemas.opengis.net/ows/1.1.0/owsExceptionReport.xsd\" version=\"1.1.0\" xml:lang=\"en\">";
    static char postamble[]="\n</ExceptionReport>" ;

    ap_set_content_type(r,"text/xml");
    ap_rputs(preamble, r);

    int i;
    for(i = 0; i < errors; i++)
    {
        wmts_error error = wmts_errors[i];

        static char preexception[]="\n<Exception exceptionCode=\"";
        static char prelocator[]="\" locator=\"";
        static char postlocator[]="\">";
        static char pretext[]="\n<ExceptionText>";
        static char posttext[]="</ExceptionText>";
        static char postexception[]="</Exception>";

        ap_rputs(preexception, r);
        ap_rputs(error.exceptionCode, r);
        ap_rputs(prelocator, r);
        ap_rputs(error.locator, r);
        ap_rputs(postlocator, r);
        ap_rputs(pretext, r);
        ap_rputs(error.exceptionText, r);
        ap_rputs(posttext, r);
        ap_rputs(postexception, r);

        r->status = error.status;
        error.exceptionCode = 0;
    }

    ap_rputs(postamble, r);
    return OK; // Request handled
}

static apr_array_header_t* tokenize(apr_pool_t *p, const char *s, char sep)
{
    apr_array_header_t* arr = apr_array_make(p, 10, sizeof(char *));
    while (sep == *s) s++;
    char *val;
    while (*s && (val = ap_getword(p, &s, sep))) {
        char **newelt = (char **)apr_array_push(arr);
        *newelt = val;
    }
    return arr;
}

static const char *find_and_replace_string(apr_pool_t *p, const char *search_str, const char *source_str, const char *replacement_str) {
    if (const char *replacefield = ap_strstr(source_str, search_str)) {
        const char *prefix = apr_pstrmemdup(p, source_str, replacefield - source_str);
        return apr_pstrcat(p, prefix, replacement_str, replacefield + strlen(search_str), NULL);
    }
    return source_str;
}

static const char *add_date_to_filename(apr_pool_t *p, const char *source_str, const char *date_str)
{
    struct tm req_time = {0};
    strptime(date_str, "%Y-%m-%d", &req_time);
    char *out_uri = (char *)apr_pcalloc(p, MAX_STRING_LEN);
    strftime(out_uri, MAX_STRING_LEN, source_str, &req_time);
    return out_uri;
}


static const char *remove_date_from_uri(apr_pool_t *p, apr_array_header_t *tokens)
{
    int i;
    char *out_uri = (char *)apr_pcalloc(p, MAX_STRING_LEN);
    char *ptr = out_uri;
    for (i=0; i<tokens->nelts; i++) {
        if (i == tokens->nelts - 5) continue;
        *ptr++ = '/';
        const char *token = (const char *)APR_ARRAY_IDX(tokens, i, const char *);
        apr_cpystrn(ptr, token, MAX_STRING_LEN);
        ptr += strlen(token);
    }
    return out_uri;
}

static const char *get_blank_tile_filename(request_rec *r)
{
    const char *blank_tile_filename;
    const char *uri = r->uri;
    const char *file_ext = uri + strlen(uri) - 4;
    apr_table_t *args_table;
    ap_args_to_table(r, &args_table);
    const char *param = apr_table_get(args_table, "FORMAT");
    if (apr_strnatcasecmp(param, "image/jpeg") == 0 || apr_strnatcasecmp(file_ext, ".jpg") == 0)  {
        blank_tile_filename = "black.jpg";
    } else if (apr_strnatcasecmp(param, "image/png") == 0 || apr_strnatcasecmp(file_ext, ".png") == 0)  {
        blank_tile_filename = "transparent.png";
    } else if (apr_strnatcasecmp(param, "application/vnd.mapbox-vector-tile") == 0 || apr_strnatcasecmp(file_ext, ".mvt") == 0)  {
        blank_tile_filename = "empty.mvt";
    } else if (apr_strnatcasecmp(param, "application/x-protobuf;type=mapbox-vector") == 0 || apr_strnatcasecmp(file_ext, ".pbf") == 0)  {
        blank_tile_filename = "empty.mvt";
    } else {
        return NULL;
    }
    return apr_psprintf(r->pool, "%s/%s", get_base_uri(r), blank_tile_filename);
}

static int handleKvP(request_rec *r) 
{
    wmts_error wmts_errors[10];
    int errors = 0;
    wmts_wrapper_conf *cfg = (wmts_wrapper_conf *)ap_get_module_config(r->per_dir_config, &wmts_wrapper_module); 
    apr_table_t *args_table;
    ap_args_to_table(r, &args_table);

    const char *param = NULL;
    const char *version = NULL;
    if ((param = apr_table_get(args_table, "VERSION")) && strlen(param)) {
        if (apr_strnatcasecmp(param, "1.0.0") == 0) {
            version = param;
        } else {
            wmts_errors[errors++] = wmts_make_error(400,"InvalidParameterValue","VERSION", "VERSION is invalid");
        }
    } else {
        wmts_errors[errors++] = wmts_make_error(400,"MissingParameterValue","VERSION", "Missing VERSION parameter");
    }

    const char *style = NULL;
    if ((param = apr_table_get(args_table, "STYLE")) && strlen(param)) {
        style = param;
    } else {
        style = "default";
    }

    const char *time = NULL;
    if ((param = apr_table_get(args_table, "TIME")) && strlen(param)) {
        // Verify that the date is in the right format
        if (ap_regexec(cfg->date_regexp, param, 0, NULL, 0) == AP_REG_NOMATCH 
            && apr_strnatcasecmp(param, "default")) {
            wmts_errors[errors++] = wmts_make_error(400,"InvalidParameterValue","TIME", "Invalid time format, must be YYYY-MM-DD or YYYY-MM-DDThh:mm:ssZ");
        } else {
            time = param;
        }
    }

    const char *request = NULL;
    if ((param = apr_table_get(args_table, "REQUEST")) && strlen(param)) {
        if (apr_strnatcasecmp(param, "GetCapabilities") != 0 && apr_strnatcasecmp(param, "GetTile") != 0 && apr_strnatcasecmp(param, "GetTileService") != 0) {
            wmts_errors[errors++] = wmts_make_error(501, "OperationNotSupported","REQUEST", "The request type is not supported");
        } else {
            request = param;
        }
    } else {
        wmts_errors[errors++] = wmts_make_error(400,"MissingParameterValue","REQUEST", "Missing REQUEST parameter");
    }  
    
    const char *layer = NULL;
    if ((param = apr_table_get(args_table, "LAYER")) && strlen(param)) {
        layer = param;
    } else {
        if (request && apr_strnatcasecmp(request, "GetCapabilities") != 0 && apr_strnatcasecmp(request, "GetTileService") != 0) {
            wmts_errors[errors++] = wmts_make_error(400,"MissingParameterValue","LAYER", "Missing LAYER parameter");
        }
    }

    const char *service = NULL;
    if (((param = apr_table_get(args_table, "SERVICE")) || (param = apr_table_get(args_table, "wmts.cgi?SERVICE"))) && strlen(param)) { // mod_onearth is doing weird things with the arguments list
        if (apr_strnatcasecmp(param, "WMTS"))
            wmts_errors[errors++] = wmts_make_error(400,"InvalidParameterValue","SERVICE", "Unrecognized service");
    } else {
        wmts_errors[errors++] = wmts_make_error(400,"MissingParameterValue","SERVICE", "Missing SERVICE parameter");
    }   

    const char *format = NULL;
    if (request && apr_strnatcasecmp(request, "GetTile") == 0) {
        if ((param = apr_table_get(args_table, "FORMAT")) && strlen(param)) {
            if (apr_strnatcasecmp(param, "image/jpeg") == 0) {
                format = ".jpg";
            } else if (apr_strnatcasecmp(param, "image/png") == 0) {
                format = ".png";
            } else if (apr_strnatcasecmp(param, "image/tiff") == 0) {
                format = ".tiff";
            } else if (apr_strnatcasecmp(param, "image/lerc") == 0) {
                format = ".lerc";
            } else if (apr_strnatcasecmp(param, "application/x-protobuf;type=mapbox-vector") == 0) {
                format = ".pbf";
            } else if (apr_strnatcasecmp(param, "application/vnd.mapbox-vector-tile") == 0) {
                format = ".mvt";
            } else {
                wmts_errors[errors++] = wmts_make_error(400,"InvalidParameterValue","FORMAT", "FORMAT is invalid for LAYER");
            }
        } else {
            wmts_errors[errors++] = wmts_make_error(400,"MissingParameterValue","FORMAT", "Missing FORMAT parameter");
        }

        const char *tilematrixset = NULL;
        if ((param = apr_table_get(args_table, "TILEMATRIXSET")) && strlen(param)) {
            tilematrixset = param;
        } else {
            wmts_errors[errors++] = wmts_make_error(400,"MissingParameterValue","TILEMATRIXSET", "Missing TILEMATRIXSET parameter");
        }

        const char *tile_l = NULL;
        if ((param = apr_table_get(args_table, "TILEMATRIX")) && strlen(param)) {
            tile_l = param;
        } else {
            wmts_errors[errors++] = wmts_make_error(400,"MissingParameterValue","TILEMATRIX", "Missing TILEMATRIX parameter");
        }

        const char *tile_x = NULL;
        if ((param = apr_table_get(args_table, "TILEROW")) && strlen(param)) {
            tile_x = param;
        } else {
            wmts_errors[errors++] = wmts_make_error(400,"MissingParameterValue","TILEROW", "Missing TILEROW parameter");
        }

        const char *tile_y = NULL;
        if ((param = apr_table_get(args_table, "TILECOL")) && strlen(param)) {
            tile_y = param;
        } else {
            wmts_errors[errors++] = wmts_make_error(400,"MissingParameterValue","TILECOL", "Missing TILECOL parameter");
        }

        if (errors) {
            return wmts_return_all_errors(r, errors, wmts_errors);
        }

        const char *out_uri = apr_psprintf(r->pool, "%s/%s/%s/%s/%s/%s/%s%s",
                                        get_base_uri(r),
                                        layer,
                                        style,
                                        tilematrixset,
                                        tile_l,
                                        tile_x,
                                        tile_y,
                                        format
                                        );
        apr_table_set(r->notes, "mod_wmts_wrapper_date", time ? time : "default");
        ap_internal_redirect(out_uri, r);
        return OK;
    } else if (request && apr_strnatcasecmp(request, "GetCapabilities") == 0) {
        ap_internal_redirect(apr_psprintf(r->pool, "%s/%s", get_base_uri(r), "getCapabilities.xml"), r);
        return DECLINED;    
    } else if (request && apr_strnatcasecmp(request, "GetTileService") == 0) {
        ap_internal_redirect(apr_psprintf(r->pool, "%s/%s", get_base_uri(r), "getTileService.xml"), r);
        return DECLINED;    
    }
    if (errors) {
        return wmts_return_all_errors(r, errors, wmts_errors);
    }
    return DECLINED;
}

static int get_filename_and_date_from_date_service(request_rec *r, wmts_wrapper_conf *cfg, const char *layer_name, const char *datetime_str, char **filename, char **date_string) {
    // Rewrite URI to exclude date and put the date in the notes for the redirect.
    ap_filter_rec_t *receive_filter = ap_get_output_filter_handle("Receive");
    SERR_IF(!receive_filter, "Mod_wmts_wrapper needs mod_receive to be available to make time queries");

    // Allocate buffer for JSON response
    receive_ctx rctx;
    rctx.maxsize = 1024*1024;
    rctx.buffer = (char *)apr_palloc(r->pool, rctx.maxsize);
    rctx.size = 0;

    const char* time_request_uri = apr_psprintf(r->pool, "%s?layer=%s&datetime=%s", cfg->time_lookup_uri, layer_name, datetime_str);
    ap_filter_t *rf = ap_add_output_filter_handle(receive_filter, &rctx, r, r->connection);
    request_rec *rr = ap_sub_req_lookup_uri(time_request_uri, r, r->output_filters);

    // LOGGING
    const char *uuid = apr_table_get(r->headers_in, "UUID") 
        ? apr_table_get(r->headers_in, "UUID") 
        : apr_table_get(r->subprocess_env, "UNIQUE_ID");
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "step=begin_request_time_snap, timestamp=%ld, uuid=%s",
        apr_time_now(), uuid);
    apr_table_set(rr->headers_out, "UUID", uuid);

    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "step=begin_send_to_date_service, timestamp=%ld, uuid=%s",
      apr_time_now(), uuid);

    int rr_status = ap_run_sub_req(rr);
    ap_remove_output_filter(rf);
    if (rr_status != APR_SUCCESS) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, rr_status, r, "Time lookup failed for %s", time_request_uri);
        return rr_status; // Pass status along
    }

    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "step=end_send_to_date_service, timestamp=%ld, uuid=%s",
      apr_time_now(), uuid);
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "step=end_return_to_wrapper, timestamp=%ld, uuid=%s",
      apr_time_now(), uuid);

    json_error_t *error = (json_error_t *)apr_pcalloc(r->pool, MAX_STRING_LEN);
    json_t *root = json_loadb(rctx.buffer, rctx.size, 0, error);
    json_unpack(root, "{s:s, s:s}", "date", date_string, "filename", filename);

    return APR_SUCCESS;
}

// static const char *get_date_from_uri(apr_pool_t *p, wmts_wrapper_conf *cfg, const char *uri)
// {
//     const char *pattern = "\\d{4}-\\d{2}-\\d{2}";
//     ap_regex_t *time_regexp = (ap_regex_t *)apr_palloc(p, sizeof(ap_regex_t));
//     ap_regmatch_t matches[AP_MAX_REG_MATCH];
//     ap_regcomp(time_regexp, pattern, 0);
//     if (ap_regexec(time_regexp, uri, AP_MAX_REG_MATCH, matches, 0) != AP_REG_NOMATCH) {
//         return apr_pstrmemdup(p, uri + matches[0].rm_so, matches[0].rm_eo - matches[0].rm_so);
//     }
//     return "";    
// }

/* The pre-hook function does a few things. 

-- For requests with a date, it verifies that the date is good, then adds the date to the request 
notes while stripping it from the URI. It then redirects the request to the URI without a date. 
This allows us to keep a flat directory structure for the configs despite the fact that
the date param is always changing. 

-- Then, when the request comes back around (this time into the TMS directory), we grab the configuration for
mod_reproject, modify it with a source path that includes the date, and put the new config in the request_config
area.

-- We also do a basic check to see if the tile request is within the accepted dimensions for this TMS.
This is possible because we're grabbing the mod_reproject configuration and getting those values from it.
*/
static int pre_hook(request_rec *r)
{
    char *err_msg;
    wmts_error wmts_errors[5];
    int errors = 0;
    int status;
    const char *datetime_str;

    wmts_wrapper_conf *cfg = (wmts_wrapper_conf *)ap_get_module_config(r->per_dir_config, &wmts_wrapper_module);   
    if (!cfg->role) return DECLINED;

    if (apr_strnatcasecmp(cfg->role, "root") == 0) {
        return DECLINED;
    } else if (apr_strnatcasecmp(cfg->role, "style") == 0 && cfg->time) {
        // If we've already handled the date, but are still getting stuck at the STYLE part of the REST request, we know the TMS is bad.
        if (apr_table_get(r->notes, "mod_wmts_wrapper_filename") || (r->prev && apr_table_get(r->prev->notes, "mod_wmts_wrapper_date"))) {
            wmts_errors[errors++] = wmts_make_error(400,"InvalidParameterValue","TILEMATRIXSET", "TILEMATRIXSET is invalid for LAYER");
            return wmts_return_all_errors(r, errors, wmts_errors);
        }

        // Date handling -- verify date, then add it to the notes and re-issue the request sans date
        apr_array_header_t *tokens = tokenize(r->pool, r->uri, '/');
        datetime_str = (char *)APR_ARRAY_IDX(tokens, tokens->nelts - 5, char *);
        if (ap_regexec(cfg->date_regexp, datetime_str, 0, NULL, 0) == AP_REG_NOMATCH 
            && apr_strnatcasecmp(datetime_str, "default")) {
            wmts_errors[errors++] = wmts_make_error(400,"InvalidParameterValue","TIME", "Invalid time format, must be YYYY-MM-DD or YYYY-MM-DDThh:mm:ssZ");
        }

        apr_table_set(r->notes, "mod_wmts_wrapper_date", datetime_str);        
        if (errors) return wmts_return_all_errors(r, errors, wmts_errors);
        const char *out_uri = remove_date_from_uri(r->pool, tokens);
        ap_internal_redirect(out_uri, r);

        return DECLINED;
    } else if (apr_strnatcasecmp(cfg->role, "tilematrixset") == 0) {
        const char *uuid = apr_table_get(r->headers_in, "UUID") 
            ? apr_table_get(r->headers_in, "UUID") 
            : apr_table_get(r->subprocess_env, "UNIQUE_ID");
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "step=begin_onearth_handle, timestamp=%ld, uuid=%s",
            apr_time_now(), uuid);


        const char *filename = r->prev ? apr_table_get(r->prev->notes, "mod_wmts_wrapper_filename") : NULL;
        datetime_str = r->prev ? apr_table_get(r->prev->notes, "mod_wmts_wrapper_date") : NULL;
        
        // Start by verifying the requested tile coordinates/format from the URI against the mod_reproject configuration for this endpoint
        apr_array_header_t *tokens = tokenize(r->pool, r->uri, '/');
        const char *dim;

        dim = *(char **)apr_array_pop(tokens);
        const char *extension = ap_strchr(dim, '.');
        if  (extension && cfg->mime_type) {
            if (!check_valid_extension(cfg, extension)) {
                err_msg = "FORMAT is invalid for LAYER";
                wmts_errors[errors++] = wmts_make_error(400, "InvalidParameterValue","FORMAT", err_msg);
            }
        }

        if (!isdigit(*dim)) wmts_errors[errors++] = wmts_make_error(400, "InvalidParameterValue","TILECOL", "TILECOL is not a valid integer");
        int tile_x = apr_atoi64(dim);

        dim = *(char **)apr_array_pop(tokens);
        if (!isdigit(*dim)) wmts_errors[errors++] = wmts_make_error(400, "InvalidParameterValue","TILEROW", "TILEROW is not a valid integer");
        int tile_y = apr_atoi64(dim);
        
        dim = *(char **)apr_array_pop(tokens);
        if (!isdigit(*dim)) wmts_errors[errors++] = wmts_make_error(400, "InvalidParameterValue","TILEMATRIX", "TILEMATRIX is not a valid integer");
        int tile_l = apr_atoi64(dim);

        // Get AHTSE module configs (if available)
        module *reproject_module = (module *)ap_find_linked_module("mod_reproject.cpp");
        repro_conf *reproject_config = reproject_module 
            ? (repro_conf *)ap_get_module_config(r->per_dir_config, reproject_module)
            : NULL;

        module *mrf_module = (module *)ap_find_linked_module("mod_mrf.cpp");
        mrf_conf *mrf_config = mrf_module
            ? (mrf_conf *)ap_get_module_config(r->per_dir_config, mrf_module)
            : NULL;

        int n_levels = (reproject_config && reproject_config->raster.n_levels) ? reproject_config->raster.n_levels 
            : (mrf_config && mrf_config->n_levels) ? mrf_config->n_levels : NULL;
        if (n_levels) {
            // Get the tile grid bounds from the tile module config and compare them with the requested tile.
            char *err_msg;
            int max_width;
            int max_height;
            if (tile_l > n_levels || tile_l < 0) {
                err_msg = apr_psprintf(r->pool, "TILEMATRIX is out of range, maximum value is %d", n_levels - 1);
                wmts_errors[errors++] = wmts_make_error(400, "TileOutOfRange","TILEMATRIX", err_msg);
            } 
            if (reproject_config && reproject_config->raster.rsets) {
                rset *rsets = reproject_config->raster.rsets;
                max_width = rsets[tile_l].width;
                max_height = rsets[tile_l].height;
            } else if (mrf_config && mrf_config->rsets) {
                tile_l += mrf_config->skip_levels;
                mrf_rset *rsets = mrf_config->rsets;
                max_width = rsets[tile_l].width;
                max_height = rsets[tile_l].height;
            }

            if (tile_x >= max_width || tile_x < 0) {
                err_msg = apr_psprintf(r->pool, "TILECOL is out of range, maximum value is %d", max_width - 1);
                wmts_errors[errors++] = wmts_make_error(400, "TileOutOfRange","TILECOL", err_msg);
            } else if (tile_y >= max_height || tile_y < 0) {
                err_msg = apr_psprintf(r->pool, "TILEROW is out of range, maximum value is %d", max_height - 1);
                wmts_errors[errors++] = wmts_make_error(400, "TileOutOfRange","TILEROW", err_msg);
            }
            if (errors) return wmts_return_all_errors(r, errors, wmts_errors);
            if (cfg->time) {
                if (reproject_config && reproject_config->source) {
                    repro_conf *out_cfg = (repro_conf *)apr_palloc(r->pool, sizeof(repro_conf));
                    memcpy(out_cfg, reproject_config, sizeof(repro_conf));
                    out_cfg->source = find_and_replace_string(r->pool, "${date}", reproject_config->source, datetime_str);
                    ap_set_module_config(r->request_config, reproject_module, out_cfg);  
                } else if (mrf_config && (mrf_config->datafname || mrf_config->redirect)) {
                    apr_array_pop(tokens); // Discard TMS name
                    apr_array_pop(tokens); // Discard style name
                    // Get filename from date service and amend the mod_mrf config to point to it
                    char *layer_name = *(char **)apr_array_pop(tokens);
                    char *filename = (char *)apr_pcalloc(r->pool, MAX_STRING_LEN);
                    char *date_string = (char *)apr_pcalloc(r->pool, MAX_STRING_LEN);

                    status = get_filename_and_date_from_date_service(r, cfg, layer_name, datetime_str, &filename, &date_string);
                    if (status != APR_SUCCESS) return status;

                    mrf_conf *out_cfg = (mrf_conf *)apr_palloc(r->pool, sizeof(mrf_conf));
                    memcpy(out_cfg, mrf_config, sizeof(mrf_conf));
                    if (mrf_config->datafname) {
                        out_cfg->datafname = (char *)find_and_replace_string(r->pool, "${filename}", mrf_config->datafname, filename);
                    }
                    if (mrf_config->redirect) {
                        out_cfg->redirect = (char *)find_and_replace_string(r->pool, "${filename}", mrf_config->redirect, filename);               
                    }
                    out_cfg->idxfname = (char *)find_and_replace_string(r->pool, "${filename}", mrf_config->idxfname, filename);
                    // Add the year dir to the IDX filename if that option is configured
                    if (cfg->year_dir) {
                        const char *year = apr_pstrndup(r->pool, date_string, 4);
                        out_cfg->idxfname = (char *)find_and_replace_string(r->pool, "${YYYY}", out_cfg->idxfname, year);
                    }
                    ap_set_module_config(r->request_config, mrf_module, out_cfg);  
                }
            }
        }
        if (errors) return wmts_return_all_errors(r, errors, wmts_errors);
    }
    return DECLINED;
}


/* The handler is set to run at the very end of the stack. Essentially, if a request hasn't been 
picked up by this point, we know that something is wrong with it and use the Role tag to determine 
what that is.
*/
static int post_hook(request_rec *r)
{
    wmts_error wmts_errors[5];
    int errors = 0;

    wmts_wrapper_conf *cfg = (wmts_wrapper_conf *)ap_get_module_config(r->per_dir_config, &wmts_wrapper_module);
    if (!cfg->role) return DECLINED;

    // First check to see if request is for a valid file.
    apr_finfo_t *fileinfo = (apr_finfo_t *)apr_palloc(r->pool, sizeof(apr_finfo_t));
    if (apr_stat(fileinfo, r->filename, 0, r->pool) == APR_SUCCESS) return DECLINED;

    if (!apr_strnatcasecmp(cfg->role, "root")) {
        // If mod_onearth has handled and failed this request, we serve up the appropriate blank tile (if it exists)
        if (apr_table_get(r->notes, "mod_onearth_failed")) {
            if (const char *blank_tile_url = get_blank_tile_filename(r)) {
                ap_internal_redirect(blank_tile_url, r);
            }
            return DECLINED;
        }

        if (r->args) return handleKvP(r);

        wmts_errors[errors++] = wmts_make_error(400, "InvalidParameterValue", "LAYER", "LAYER does not exist");
        return wmts_return_all_errors(r, errors, wmts_errors);
    } 

    if (!apr_strnatcasecmp(cfg->role, "style") && cfg->time) {



        // const char *out_uri = remove_date_from_uri(r->pool, tokens);
        // apr_table_set(r->notes, "mod_wmts_wrapper_date", datetime_str);
        // ap_internal_redirect(out_uri, r);
        // return DECLINED;
    }

    if (!apr_strnatcasecmp(cfg->role, "layer")) {

        // If we've already handled a date for this request, we know that it's a TMS error and not a STYLE error
        if (r->prev && apr_table_get(r->prev->notes, "mod_wmts_wrapper_date") && !r->prev->args) {
            wmts_errors[errors++] = wmts_make_error(400,"InvalidParameterValue","TILEMATRIXSET", "TILEMATRIXSET is invalid for LAYER");
        } else {
            wmts_errors[errors++] = wmts_make_error(400,"InvalidParameterValue","STYLE", "STYLE is invalid for LAYER");
        }
        return wmts_return_all_errors(r, errors, wmts_errors);
    } 
    if (!apr_strnatcasecmp(cfg->role, "style")) {
        wmts_errors[errors++] = wmts_make_error(400,"InvalidParameterValue","TILEMATRIXSET", "TILEMATRIXSET is invalid for LAYER");
        return wmts_return_all_errors(r, errors, wmts_errors);
    } 
    if (!apr_strnatcasecmp(cfg->role, "tilematrixset")) {
        // This would be a tile-level request and as such errors are handled by the pre-hook.
    }
    return DECLINED;
}


static const char *set_module(cmd_parms *cmd, void *dconf, const char *role)
{
    wmts_wrapper_conf *cfg = (wmts_wrapper_conf *)dconf;
    cfg->role = apr_pstrdup(cmd->pool, role);
    const char *pattern = "^\\d{4}-\\d{2}-\\d{2}$|^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}Z$";
    cfg->date_regexp = (ap_regex_t *)apr_palloc(cmd->pool, sizeof(ap_regex_t));
    if (ap_regcomp(cfg->date_regexp, pattern, 0)) {
        return "Error -- bad date regexp";
    }
    return NULL;    
}

static const char *enable_time(cmd_parms *cmd, void *dconf, int arg)
{
    wmts_wrapper_conf *cfg = (wmts_wrapper_conf *)dconf;
    cfg->time = arg;
	return NULL;
}

static const char *enable_year_dir(cmd_parms *cmd, void *dconf, int arg)
{
    wmts_wrapper_conf *cfg = (wmts_wrapper_conf *)dconf;
    cfg->year_dir = arg;
    return NULL;
}

static const char *set_mime_type(cmd_parms *cmd, void *dconf, const char *format)
{
    wmts_wrapper_conf *cfg = (wmts_wrapper_conf *)dconf;
    cfg->mime_type = apr_pstrdup(cmd->pool, format);
    return NULL;
}

static const char *set_time_lookup(cmd_parms *cmd, void *dconf, const char *uri)
{
    wmts_wrapper_conf *cfg = (wmts_wrapper_conf *)dconf;
    cfg->time_lookup_uri = apr_pstrdup(cmd->pool, uri);
    return NULL;
}

static void *create_dir_config(apr_pool_t *p, char *unused)
{
    return apr_pcalloc(p, sizeof(wmts_wrapper_conf));
}

static void* merge_dir_conf(apr_pool_t *p, void *BASE, void *ADD) {
    wmts_wrapper_conf *base = (wmts_wrapper_conf *)BASE;
    wmts_wrapper_conf *add = (wmts_wrapper_conf *)ADD;
    wmts_wrapper_conf *cfg = (wmts_wrapper_conf *)apr_palloc(p, sizeof(wmts_wrapper_conf));
    cfg->role = ( add->role == NULL ) ? base->role : add->role;
    cfg->time = ( add->time == NULL ) ? base->time : add->time;
    cfg->date_regexp = ( add->date_regexp == NULL ) ? base->date_regexp : add->date_regexp;
    cfg->mime_type = ( add->mime_type == NULL ) ? base->mime_type : add->mime_type;
    cfg->time_lookup_uri = ( add->time_lookup_uri == NULL ) ? base->time_lookup_uri : add->time_lookup_uri;
    cfg->year_dir = ( add->year_dir == NULL ) ? base->year_dir : add->year_dir;
    return cfg;
}

static void register_hooks(apr_pool_t *p)

{
    ap_hook_handler(pre_hook, NULL, NULL, APR_HOOK_FIRST-1);
    ap_hook_handler(post_hook, NULL, NULL, APR_HOOK_LAST);
}

static const command_rec cmds[] = 
{
    AP_INIT_TAKE1(
        "WMTSWrapperRole",
        (cmd_func) set_module, // Callback
        0, // Self pass argument
        ACCESS_CONF,
        "Set role for the WMTS module in this <Directory> block"
    ),

    AP_INIT_FLAG(
        "WMTSWrapperEnableTime",
        (cmd_func) enable_time, // Callback
        0, // Self pass argument
        ACCESS_CONF,
        "Enable Time handling for WMTS wrapper module for this layer directory"
    ),

    AP_INIT_TAKE1(
        "WMTSWrapperMimeType",
        (cmd_func) set_mime_type, // Callback
        0, // Self pass argument
        ACCESS_CONF,
        "Set MIME for the WMTS module in this <Directory> block"
    ),

    AP_INIT_TAKE1(
        "WMTSWrapperTimeLookupUri",
        (cmd_func) set_time_lookup, // Callback
        0, // Self pass argument
        ACCESS_CONF,
        "Set URI for time lookup service"
    ),

    AP_INIT_FLAG(
        "WMTSWrapperEnableYearDir",
        (cmd_func) enable_year_dir, // Callback
        0, // Self pass argument
        ACCESS_CONF,
        "Add year directories when looking up index files"
    ),

    {NULL}
};

module AP_MODULE_DECLARE_DATA wmts_wrapper_module = {
    STANDARD20_MODULE_STUFF,
    create_dir_config,
    merge_dir_conf,
    0, // No server_config
    0, // No server_merge
    cmds, // configuration directives
    register_hooks // processing hooks
};
