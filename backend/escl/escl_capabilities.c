/* sane - Scanner Access Now Easy.

   Copyright (C) 2019 Touboul Nathane
   Copyright (C) 2019 Thierry HUCHARD <thierry@ordissimo.com>

   This file is part of the SANE package.

   SANE is free software; you can redistribute it and/or modify it under
   the terms of the GNU General Public License as published by the Free
   Software Foundation; either version 3 of the License, or (at your
   option) any later version.

   SANE is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
   FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
   for more details.

   You should have received a copy of the GNU General Public License
   along with sane; see the file COPYING.  If not, write to the Free
   Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

   This file implements a SANE backend for eSCL scanners.  */

#define DEBUG_DECLARE_ONLY
#include "../include/sane/config.h"

#include "escl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <libxml/parser.h>

#include "../include/sane/saneopts.h"

struct cap
{
    char *memory;
    size_t size;
};

/**
 * \fn static SANE_String_Const convert_elements(SANE_String_Const str)
 * \brief Function that converts the 'color modes' of the scanner (color/gray) to be understood by SANE.
 *
 * \return SANE_VALUE_SCAN_MODE_GRAY / SANE_VALUE_SCAN_MODE_COLOR / SANE_VALUE_SCAN_MODE_LINEART; NULL otherwise
 */
static SANE_String_Const
convert_elements(SANE_String_Const str)
{
    if (strcmp(str, "Grayscale8") == 0)
        return (SANE_VALUE_SCAN_MODE_GRAY);
    else if (strcmp(str, "RGB24") == 0)
        return (SANE_VALUE_SCAN_MODE_COLOR);
#if(defined HAVE_POPPLER_GLIB)
    else if (strcmp(str, "BlackAndWhite1") == 0)
        return (SANE_VALUE_SCAN_MODE_LINEART);
#endif
    return (NULL);
}

/**
 * \fn static SANE_String_Const *char_to_array(SANE_String_Const *tab, int *tabsize, SANE_String_Const mode, int good_array)
 * \brief Function that creates the character arrays to put inside :
 *        the 'color modes', the 'content types', the 'document formats' and the 'supported intents'.
 *
 * \return board (the allocated array)
 */
static SANE_String_Const *
char_to_array(SANE_String_Const *tab, int *tabsize, SANE_String_Const mode, int good_array)
{
    SANE_String_Const *board = NULL;
    int i = 0;
    SANE_String_Const convert = NULL;

    if (mode == NULL)
        return (tab);
    if (good_array != 0) {
        convert = convert_elements(mode);
        if (convert == NULL)
            return (tab);
    }
    else
        convert = mode;
    for (i = 0; i < (*tabsize); i++) {
        if (strcmp(tab[i], convert) == 0)
            return (tab);
    }
    (*tabsize)++;
    if (*tabsize == 1)
        board = (SANE_String_Const *)malloc(sizeof(SANE_String_Const) * (*tabsize) + 1);
    else
        board = (SANE_String_Const *)realloc(tab, sizeof(SANE_String_Const) * (*tabsize) + 1);
    board[*tabsize - 1] = (SANE_String_Const)strdup(convert);
    board[*tabsize] = NULL;
    return (board);
}

/**
 * \fn static SANE_Int *int_to_array(SANE_Int *tab, int *tabsize, int cont)
 * \brief Function that creates the integer array to put inside the 'supported resolutions'.
 *
 * \return board (the allocated array)
 */
static SANE_Int *
int_to_array(SANE_Int *tab, int *tabsize, int cont)
{
    SANE_Int *board = NULL;
    int i = 0;

    for (i = 0; i < (*tabsize); i++) {
        if (tab[i] == cont)
            return (tab);
    }
    (*tabsize)++;
    if (*tabsize == 1) {
        (*tabsize)++;
        board = malloc(sizeof(SANE_Int *) * (*tabsize) + 1);
    }
    else
        board = realloc(tab, sizeof(SANE_Int *) * (*tabsize) + 1);
    board[0] = *tabsize - 1;
    board[*tabsize - 1] = cont;
    board[*tabsize] = -1;
    return (board);
}

/**
 * \fn static size_t memory_callback_c(void *contents, size_t size, size_t nmemb, void *userp)
 * \brief Callback function that stocks in memory the content of the scanner capabilities.
 *
 * \return realsize (size of the content needed -> the scanner capabilities)
 */
static size_t
memory_callback_c(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    struct cap *mem = (struct cap *)userp;

    char *str = realloc(mem->memory, mem->size + realsize + 1);
    if (str == NULL) {
        fprintf(stderr, "not enough memory (realloc returned NULL)\n");
        return (0);
    }
    mem->memory = str;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size = mem->size + realsize;
    mem->memory[mem->size] = 0;
    return (realsize);
}

/**
 * \fn static int find_nodes_c(xmlNode *node)
 * \brief Function that browses the xml file and parses it, to find the xml children node.
 *        --> to recover the scanner capabilities.
 *
 * \return 0 if a xml child node is found, 1 otherwise
 */
static int
find_nodes_c(xmlNode *node)
{
    xmlNode *child = node->children;

    while (child) {
        if (child->type == XML_ELEMENT_NODE)
            return (0);
        child = child->next;
    }
    return (1);
}

/**
 * \fn static int find_valor_of_array_variables(xmlNode *node, capabilities_t *scanner)
 * \brief Function that searchs in the xml file if a scanner capabilitie stocked
 *        in one of the created array (character/integer array) is found.
 *
 * \return 0
 */
static int
find_valor_of_array_variables(xmlNode *node, capabilities_t *scanner, int type)
{
    const char *name = (const char *)node->name;
    if (strcmp(name, "ColorMode") == 0) {
		const char *color = (SANE_String_Const)xmlNodeGetContent(node);
		if (type == PLATEN || strcmp(color, "BlackAndWhite1"))
          scanner->caps[type].ColorModes = char_to_array(scanner->caps[type].ColorModes, &scanner->caps[type].ColorModesSize, (SANE_String_Const)xmlNodeGetContent(node), 1);
	}
    else if (strcmp(name, "ContentType") == 0)
        scanner->caps[type].ContentTypes = char_to_array(scanner->caps[type].ContentTypes, &scanner->caps[type].ContentTypesSize, (SANE_String_Const)xmlNodeGetContent(node), 0);
    else if (strcmp(name, "DocumentFormat") == 0)
     {
        int i = 0;
        SANE_Bool have_jpeg = SANE_FALSE, have_png = SANE_FALSE, have_tiff = SANE_FALSE, have_pdf = SANE_FALSE;
        scanner->caps[type].DocumentFormats = char_to_array(scanner->caps[type].DocumentFormats, &scanner->caps[type].DocumentFormatsSize, (SANE_String_Const)xmlNodeGetContent(node), 0);
        for(; i < scanner->caps[type].DocumentFormatsSize; i++)
         {
            if (!strcmp(scanner->caps[type].DocumentFormats[i], "image/jpeg"))
            {
			   have_jpeg = SANE_TRUE;
            }
#if(defined HAVE_LIBPNG)
            else if(!strcmp(scanner->caps[type].DocumentFormats[i], "image/png"))
            {
               have_png = SANE_TRUE;
            }
#endif
#if(defined HAVE_TIFFIO_H)
            else if(type == PLATEN && !strcmp(scanner->caps[type].DocumentFormats[i], "image/tiff"))
            {
               have_tiff = SANE_TRUE;
            }
#endif
#if(defined HAVE_POPPLER_GLIB)
            else if(type == PLATEN && !strcmp(scanner->caps[type].DocumentFormats[i], "application/pdf"))
            {
               have_pdf = SANE_TRUE;
            }
#endif
         }
         if (have_pdf)
             scanner->caps[type].default_format = strdup("application/pdf");
         else if (have_tiff)
             scanner->caps[type].default_format = strdup("image/tiff");
         else if (have_png)
             scanner->caps[type].default_format = strdup("image/png");
         else if (have_jpeg)
             scanner->caps[type].default_format = strdup("image/jpeg");
     }
    else if (strcmp(name, "DocumentFormatExt") == 0)
        scanner->caps[type].format_ext = 1;
    else if (strcmp(name, "Intent") == 0)
        scanner->caps[type].SupportedIntents = char_to_array(scanner->caps[type].SupportedIntents, &scanner->caps[type].SupportedIntentsSize, (SANE_String_Const)xmlNodeGetContent(node), 0);
    else if (strcmp(name, "XResolution") == 0)
        scanner->caps[type].SupportedResolutions = int_to_array(scanner->caps[type].SupportedResolutions, &scanner->caps[type].SupportedResolutionsSize, atoi((const char *)xmlNodeGetContent(node)));
    return (0);
}

/**
 * \fn static int find_value_of_int_variables(xmlNode *node, capabilities_t *scanner)
 * \brief Function that searchs in the xml file if a integer scanner capabilitie is found.
 *        The integer scanner capabilities that are interesting are :
 *        MinWidth, MaxWidth, MaxHeight, MinHeight, MaxScanRegions, MaxOpticalXResolution,
 *        RiskyLeftMargin, RiskyRightMargin, RiskyTopMargin, RiskyBottomMargin.
 *
 * \return 0
 */
static int
find_value_of_int_variables(xmlNode *node, capabilities_t *scanner, int type)
{
    int MaxWidth = 0;
    int MaxHeight = 0;
    const char *name = (const char *)node->name;

    if (strcmp(name, "MinWidth") == 0)
        scanner->caps[type].MinWidth = atoi((const char*)xmlNodeGetContent(node));
    else if (strcmp(name, "MaxWidth") == 0) {
        MaxWidth = atoi((const char*)xmlNodeGetContent(node));
        if (scanner->caps[type].MaxWidth == 0 || MaxWidth < scanner->caps[type].MaxWidth)
            scanner->caps[type].MaxWidth = atoi((const char *)xmlNodeGetContent(node));
    }
    else if (strcmp(name, "MinHeight") == 0)
        scanner->caps[type].MinHeight = atoi((const char*)xmlNodeGetContent(node));
    else if (strcmp(name, "MaxHeight") == 0) {
        MaxHeight = atoi((const char*)xmlNodeGetContent(node));
        if (scanner->caps[type].MaxHeight == 0 || MaxHeight < scanner->caps[type].MaxHeight)
            scanner->caps[type].MaxHeight = atoi((const char *)xmlNodeGetContent(node));
    }
    else if (strcmp(name, "MaxScanRegions") == 0)
        scanner->caps[type].MaxScanRegions = atoi((const char *)xmlNodeGetContent(node));
    else if (strcmp(name, "MaxOpticalXResolution") == 0)
        scanner->caps[type].MaxOpticalXResolution = atoi((const char *)xmlNodeGetContent(node));
    else if (strcmp(name, "RiskyLeftMargin") == 0)
        scanner->caps[type].RiskyLeftMargin = atoi((const char *)xmlNodeGetContent(node));
    else if (strcmp(name, "RiskyRightMargin") == 0)
        scanner->caps[type].RiskyRightMargin = atoi((const char *)xmlNodeGetContent(node));
    else if (strcmp(name, "RiskyTopMargin") == 0)
        scanner->caps[type].RiskyTopMargin = atoi((const char *)xmlNodeGetContent(node));
    else if (strcmp(name, "RiskyBottomMargin") == 0)
        scanner->caps[type].RiskyBottomMargin = atoi((const char *)xmlNodeGetContent(node));
    find_valor_of_array_variables(node, scanner, type);
    return (0);
}

/**
 * \fn static int find_true_variables(xmlNode *node, capabilities_t *scanner)
 * \brief Function that searchs in the xml file if we find a scanner capabilitie stocked
 *        in one of the created array (character/integer array),
 *        or, if we find a integer scanner capabilitie.
 *
 * \return 0
 */
static int
find_true_variables(xmlNode *node, capabilities_t *scanner, int type)
{
    const char *name = (const char *)node->name;
    if (strcmp(name, "MinWidth") == 0 ||
        strcmp(name, "MaxWidth") == 0 ||
        strcmp(name, "MinHeight") == 0 ||
        strcmp(name, "MaxHeight") == 0 ||
        strcmp(name, "MaxScanRegions") == 0 ||
        strcmp(name, "ColorMode") == 0 ||
        strcmp(name, "ContentType") == 0 ||
        strcmp(name, "DocumentFormat") == 0 ||
        strcmp(name, "XResolution") == 0 ||
        strcmp(name, "Intent") == 0 ||
        strcmp(name, "MaxOpticalXResolution") == 0 ||
        strcmp(name, "RiskyLeftMargin") == 0 ||
        strcmp(name, "RiskyRightMargin") == 0 ||
        strcmp(name, "RiskyTopMargin") == 0 ||
        strcmp(name, "RiskyBottomMargin") == 0 ||
        strcmp(name, "DocumentFormatExt") == 0)
            find_value_of_int_variables(node, scanner, type);
    return (0);
}

/**
 * \fn static int print_xml_c(xmlNode *node, capabilities_t *scanner)
 * \brief Function that browses the xml file, node by node.
 *
 * \return 0
 */
static int
print_xml_c(xmlNode *node, capabilities_t *scanner, int type)
{
    while (node) {
        if (node->type == XML_ELEMENT_NODE) {
            if (find_nodes_c(node) && type != -1)
                find_true_variables(node, scanner, type);
        }
	if (!strcmp((const char *)node->name, "PlatenInputCaps")) {
           scanner->Sources =
                 char_to_array(scanner->Sources,
                               &scanner->SourcesSize,
                               (SANE_String_Const)SANE_I18N ("Flatbed"),
                               0);
	   scanner->source = PLATEN;
           print_xml_c(node->children, scanner, PLATEN);
	   scanner->caps[PLATEN].duplex = 0;
	}
	else if (!strcmp((const char *)node->name, "AdfSimplexInputCaps")) {
           scanner->Sources =
                 char_to_array(scanner->Sources,
                               &scanner->SourcesSize,
                               (SANE_String_Const)SANE_I18N("ADF"),
                               0);
	   if (scanner->source == -1) scanner->source = ADFSIMPLEX;
           print_xml_c(node->children, scanner, ADFSIMPLEX);
	   scanner->caps[ADFSIMPLEX].duplex = 0;
	}
	else if (!strcmp((const char *)node->name, "AdfDuplexInputCaps")) {
           scanner->Sources =
                 char_to_array(scanner->Sources,
                               &scanner->SourcesSize,
                               (SANE_String_Const)SANE_I18N ("ADF Duplex"),
                               0);
	   if (scanner->source == -1) scanner->source = ADFDUPLEX;
           print_xml_c(node->children, scanner, ADFDUPLEX);
	   scanner->caps[ADFDUPLEX].duplex = 1;
	}
	else
           print_xml_c(node->children, scanner, type);
        node = node->next;
    }
    return (0);
}

static void
_reduce_color_modes(capabilities_t *scanner)
{
    int type = 0;
    for (type = 0; type < 3; type++) {
         if (scanner->caps[type].ColorModesSize) {
	     if (scanner->caps[type].default_format &&
		 strcmp(scanner->caps[type].default_format, "application/pdf")) {
                 if (scanner->caps[type].ColorModesSize == 3) {
	             free(scanner->caps[type].ColorModes);
		     scanner->caps[type].ColorModes = NULL;
	             scanner->caps[type].ColorModesSize = 0;
                     scanner->caps[type].ColorModes = char_to_array(scanner->caps[type].ColorModes,
			             &scanner->caps[type].ColorModesSize,
				     (SANE_String_Const)SANE_VALUE_SCAN_MODE_GRAY, 0);
                     scanner->caps[type].ColorModes = char_to_array(scanner->caps[type].ColorModes,
			             &scanner->caps[type].ColorModesSize,
				     (SANE_String_Const)SANE_VALUE_SCAN_MODE_COLOR, 0);
                 }
	     }
         }
    }
}

/**
 * \fn capabilities_t *escl_capabilities(const ESCL_Device *device, SANE_Status *status)
 * \brief Function that finally recovers all the capabilities of the scanner, using curl.
 *        This function is called in the 'sane_open' function and it's the equivalent of
 *        the following curl command : "curl http(s)://'ip':'port'/eSCL/ScannerCapabilities".
 *
 * \return scanner (the structure that stocks all the capabilities elements)
 */
capabilities_t *
escl_capabilities(const ESCL_Device *device, SANE_Status *status)
{
    capabilities_t *scanner = (capabilities_t*)calloc(1, sizeof(capabilities_t));
    CURL *curl_handle = NULL;
    struct cap *var = NULL;
    xmlDoc *data = NULL;
    xmlNode *node = NULL;
    const char *scanner_capabilities = "/eSCL/ScannerCapabilities";

    *status = SANE_STATUS_GOOD;
    if (device == NULL)
        *status = SANE_STATUS_NO_MEM;
    var = (struct cap *)calloc(1, sizeof(struct cap));
    if (var == NULL)
        *status = SANE_STATUS_NO_MEM;
    var->memory = malloc(1);
    var->size = 0;
    curl_handle = curl_easy_init();
    escl_curl_url(curl_handle, device, scanner_capabilities);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, memory_callback_c);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)var);
    if (curl_easy_perform(curl_handle) != CURLE_OK) {
        DBG( 1, "The scanner didn't respond.\n");
        *status = SANE_STATUS_INVAL;
    }
    data = xmlReadMemory(var->memory, var->size, "file.xml", NULL, 0);
    if (data == NULL)
        *status = SANE_STATUS_NO_MEM;
    node = xmlDocGetRootElement(data);
    if (node == NULL)
        *status = SANE_STATUS_NO_MEM;

    scanner->source = 0;
    print_xml_c(node, scanner, -1);
    _reduce_color_modes(scanner);

    xmlFreeDoc(data);
    xmlCleanupParser();
    xmlMemoryDump();
    curl_easy_cleanup(curl_handle);
    free(var->memory);
    return (scanner);
}