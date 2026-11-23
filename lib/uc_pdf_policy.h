/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * PDF viewer policy of libucwebview (library 0.6.1). GLib only, so that the rules are unit-testable on a host
 * without WPE.
 *
 * WebKit 2.54 shows a PDF with its built-in PDF.js viewer (ENABLE_PDFJS is ON for WPE, OptionsWPE.cmake:53; the
 * preference PDFJSViewerEnabled is true for GTK and WPE, UnifiedWebPreferences.yaml:6491-6506). On a Remote 3 the web
 * process crashed about 2 s after the viewer drew, 3 of 3 runs, and a host that reloads after a crash loops. So the
 * library switches the viewer off unless the process sets UC_WEBVIEW_PDF_VIEWER=1 (also true/yes/on), which
 * uc_webview_global_init() reads once:
 *
 * 1. The WebKit feature UC_PDF_VIEWER_FEATURE is set to FALSE in the settings all views share. The web process then
 *    does not create the viewer's document (DOMImplementation.cpp:175-178) and does not offer to show a PDF
 *    (WebPage.cpp:8114-8117), so WebKit's default response policy ignores it (WebKitWebView.cpp:659-662), and an
 *    <embed> or <object> of a PDF type no longer becomes a frame (WebLocalFrameLoaderClient.cpp:1896-1901).
 * 2. The view's decide-policy handler ignores every navigation response with a PDF MIME type, in the main frame and
 *    in subframes (uc_pdf_policy_check_response()). Without it text/pdf would show as a text document: WebKit shows
 *    every text/ type it does not exclude (MIMETypeRegistry.cpp:643-644).
 *
 * The viewer's own scheme webkit-pdfjs-viewer: needs no rule: WebKit loads it only into a frame whose parent is the
 * viewer's document (ResourceLoader.cpp:258-263, 923-932), which no longer exists. Checked on WebKitGTK 2.54.0: a
 * link to it fails with "Load request cancelled", an iframe of it stays about:blank, with the viewer on or off.
 */
#ifndef UC_PDF_POLICY_H
#define UC_PDF_POLICY_H

#include <glib.h>

G_BEGIN_DECLS

/* The WebKitFeature identifier of the PDF.js viewer: the preference PDFJSViewerEnabled without "Enabled"
 * (webkit_feature_get_identifier(), WebKitFeature.cpp:131-136). */
#define UC_PDF_VIEWER_FEATURE "PDFJSViewer"
/* The environment variable that keeps the viewer on, for the crash investigation. */
#define UC_PDF_VIEWER_ENV     "UC_WEBVIEW_PDF_VIEWER"

typedef enum {
    UC_PDF_POLICY_DEFAULT = 0,   /* WebKit decides as usual */
    UC_PDF_POLICY_IGNORE,        /* the library ignores the response: the load fails, the frame keeps its document */
} UcPdfVerdict;

/* TRUE when UC_WEBVIEW_PDF_VIEWER asks for the viewer ("1", "true", "yes", "on", case-insensitive). */
gboolean     uc_pdf_policy_viewer_requested (void);
/* TRUE for application/pdf and text/pdf (WebKit's PDF types, MIMETypeRegistry.cpp:263-266) and application/x-pdf;
 * case-insensitive, parameters (";charset=...") and surrounding blanks are ignored. NULL and "" are FALSE. */
gboolean     uc_pdf_policy_is_pdf_mime_type (const char *mime_type);
/* The verdict for a navigation response (main frame or subframe) with this MIME type. */
UcPdfVerdict uc_pdf_policy_check_response   (const char *mime_type, gboolean viewer_enabled);

G_END_DECLS

#endif /* UC_PDF_POLICY_H */
