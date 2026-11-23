/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * PDF viewer policy of libucwebview, see uc_pdf_policy.h.
 */
#include "uc_pdf_policy.h"

#include "uc_uri_policy.h"

#include <string.h>

static const char *const pdf_mime_types[] = { "application/pdf", "text/pdf", "application/x-pdf" };

gboolean
uc_pdf_policy_viewer_requested (void)
{
    return uc_env_value_true (g_getenv (UC_PDF_VIEWER_ENV));
}

gboolean
uc_pdf_policy_is_pdf_mime_type (const char *mime_type)
{
    const char *start, *end;

    if (mime_type == NULL)
        return FALSE;
    start = mime_type;
    while (*start == ' ' || *start == '\t')
        start++;
    end = strchr (start, ';');
    if (end == NULL)
        end = start + strlen (start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
        end--;
    for (gsize i = 0; i < G_N_ELEMENTS (pdf_mime_types); i++) {
        gsize n = strlen (pdf_mime_types[i]);
        if ((gsize) (end - start) == n && g_ascii_strncasecmp (start, pdf_mime_types[i], n) == 0)
            return TRUE;
    }
    return FALSE;
}

UcPdfVerdict
uc_pdf_policy_check_response (const char *mime_type, gboolean viewer_enabled)
{
    if (viewer_enabled)
        return UC_PDF_POLICY_DEFAULT;
    return uc_pdf_policy_is_pdf_mime_type (mime_type) ? UC_PDF_POLICY_IGNORE : UC_PDF_POLICY_DEFAULT;
}
