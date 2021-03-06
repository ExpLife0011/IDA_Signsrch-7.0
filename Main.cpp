
// ****************************************************************************
// File: Main.cpp
// Desc: Plugin main
// Auth: Sirmabus 2012
//
// ****************************************************************************
#include "stdafx.h"

#include <WaitBoxEx.h>

// Signature container
typedef struct ALIGN( 16 ) _SIG {
    LPSTR title;        // 0x00
    PBYTE data;         // 0x08
    UINT size;          // 0x10
    WORD bits;          // 0x18
    WORD flags;         // 0x1A
} SIG, *LPSIG;
typedef qvector<SIG> SIGLIST;

// Match container
typedef struct ALIGN( 16 ) _MATCH {
    ea_t address;
    size_t index;
    bool operator()( _MATCH const &a, _MATCH const &b ) { return(a.address < b.address); }
} MATCH, *LPMATCH;
typedef qvector<MATCH> MATCHLIST;

// wFlag defs
const WORD BIGENDIAN = (1 << 0); // 0 = little endian, 1 = big endian ** Don't change, this must be '1' **
const WORD REVERSE = (1 << 1); // Reverse/reflect
const WORD AND = (1 << 2); // And bits

#define SIGFILE "signsrch.xml"

// === Function Prototypes ===
static int idaapi pluginInit( );
static void idaapi pluginTerm( );
static bool idaapi pluginRun( size_t arg );
static void freeSignatureData( );
static void clearProcessSegmentBuffer( );
extern void clearPatternSearchData( );
static void clearMatchData( );

// === Data ===
static const char PLUGIN_NAME[] = "Signsrch";

ALIGN( 16 ) static SIGLIST   sigList;
ALIGN( 16 ) static MATCHLIST matchList;

static HMODULE myModule = NULL;
static int  iconID = -1;
static size_t sigDataBytes = 0;
static size_t totalMatches = 0;
static BOOL listWindowUp = FALSE;


// UI options bit flags
// *** Must be same sequence as check box options
static SBITFLAG BitF;
const static WORD OPT_ALTENDIAN = BitF.Next( );
const static WORD OPT_DEBUGOUT = BitF.Next( );
const static WORD OPT_CODESEGS = BitF.Next( );
const static WORD OPT_COMMENTS = BitF.Next( );
static BOOL altEndianSearch = FALSE;
static BOOL debugOutput = FALSE;
static BOOL includeCodeSegments = TRUE;
static BOOL placeComments = TRUE;

// Plug-in description block
extern "C" ALIGN( 16 ) plugin_t PLUGIN =
{
    IDP_INTERFACE_VERSION,
    PLUGIN_PROC,
    pluginInit,
    pluginTerm,
    pluginRun,
    PLUGIN_NAME,
    " ",
    PLUGIN_NAME,
    NULL
};

ALIGN( 16 ) static const char mainForm[] = {
    // 'Continue' instead of 'okay'
    "BUTTON YES* Continue\n" 
    // Title
    "IDA Signsrch\n"
    // Message text
    "IDA Signsrch\n"
    "Version: %A,   build: %A,   by Sirmabus\n\n"
    // checkbox -> bAltEndianSearch
    "<#Do alternate endian search in addition to the IDB's native endian.\nSearching will take about twice as long but can find additional matches in some cases. #Alternate endian search.:C>\n"
    // checkbox -> bDebugOutput
    "<#Output matches to the debugging channel so they can be viewed \nand logged by Sysinternals \"DebugView\", etc.#Output to debug channel.:C>\n"
    // checkbox -> bIncludeCodeSegments
    "<#Search code segments in addition to data segments. #Include code segments.:C>\n"
    // checkbox -> bPlaceComments
    "<#Automatically place label comments for located signatures.#Place signature comments.:C>>\n"
    // * Maintain button names hard coded in "HelpURL.h"
    "<#Click to open plugin support page.#Macromonkey forum:k:2:16::>   "
    "<#Click to open Luigi Auriemma's Signsrch page.#Luigi Signsrch page:k:2:16::>\n \n "
};

// Custom chooser icon
static const BYTE iconData[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00,
    0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
    0x00, 0x10, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0xF3, 0xFF, 0x61,
    0x00, 0x00, 0x00, 0x09, 0x70, 0x48, 0x59, 0x73, 0x00, 0x00, 0x0E,
    0xC4, 0x00, 0x00, 0x0E, 0xC4, 0x01, 0x95, 0x2B, 0x0E, 0x1B, 0x00,
    0x00, 0x00, 0x2F, 0x49, 0x44, 0x41, 0x54, 0x38, 0xCB, 0x63, 0x60,
    0x18, 0xF2, 0x80, 0x91, 0x61, 0x93, 0xE4, 0x7F, 0x74, 0xC1, 0xFF,
    0xBE, 0xCF, 0x30, 0x14, 0x3E, 0x63, 0x64, 0xC4, 0x10, 0x93, 0x66,
    0x60, 0x60, 0x64, 0xA1, 0xD4, 0x05, 0xA3, 0x06, 0x8C, 0x1A, 0x40,
    0x15, 0x03, 0x06, 0x1E, 0x00, 0x00, 0x73, 0xC1, 0x05, 0x2A, 0x17,
    0xC4, 0xDC, 0xF5, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
    0xAE, 0x42, 0x60, 0x82,
};

// ============================================================================
// Matches list window stuff
static const LPCSTR columnHeader[] = {
    "Address",
    "Size",
    "Label",
};
const int LBCOLUMNCOUNT = (sizeof( columnHeader ) / sizeof( LPCSTR ));
static int aListBColumnWidth[LBCOLUMNCOUNT] = { 15, 4, 52 }; // (9 | CHCOL_HEX)

// Sig match list
struct window_match_list : public chooser_t {
public:
    window_match_list( )
        : chooser_t( 0, LBCOLUMNCOUNT, aListBColumnWidth, columnHeader, "[ Signsrch matches ]" )
    {
        this->icon = iconID;
    }

    /// The chooser window is closed.
    virtual void idaapi closed( )
    {
        // Clean up
        clearMatchData( );
        clearPatternSearchData( );
        freeSignatureData( );
        listWindowUp = FALSE;
    }

    /// Selection changed (cursor moved).
    /// \note This callback is not supported in the txt-version.
    /// \param n  index of the new selected item
    virtual void idaapi select( ssize_t n ) const
    {
        try
        {
            jumpto( matchList[n].address );
        }
        CATCH( ) { }
    }

    /// get the number of elements in the chooser
    virtual size_t idaapi get_count( ) const
    {
        return matchList.size( );
    }

    /// get a description of an element.
    /// \param[out] cols   vector of strings. \n
    ///                    will receive the contents of each column
    /// \param[out] icon   element's icon id, -1 - no icon
    /// \param[out] attrs  element attributes
    /// \param n           element number (0..get_count()-1)
    virtual void idaapi get_row(
        qstrvec_t *cols,
        int *icon_,
        chooser_item_attrs_t *attrs,
        size_t n ) const
    {
        ea_t address = matchList[n].address;
        segment_t *seg = getseg( address );
        // Sig Address
        if (seg)
        {
            qstring name;
            get_segm_name( &name, seg );
            cols->at( 0 ).sprnt( "%s:" EAFORMAT, name.c_str( ), address );
        }
        else
        {
            cols->at( 0 ).sprnt( "unknown:" EAFORMAT, address );
        }

        // Sig size
        cols->at( 1 ).sprnt( "%04X", sigList[matchList[n].index].size );

        // Sig title
        cols->at( 2 ).insert( sigList[matchList[n].index].title );

        // Default attributes and no icon
        *attrs = chooser_item_attrs_t( );
        *icon_ = -1;
    }
};

static struct window_match_list *matchListChooser;

// ======================================================================================
static int idaapi pluginInit( )
{
    // SIG struct should be align 16
    C_ASSERT( (sizeof( SIG ) & (16 - 1)) == 0 );

    // Create custom icon
    if (iconID == -1)
        iconID = load_custom_icon( iconData, sizeof( iconData ), "png" );

    // Create list view window - DO NOT FREE THIS ALLOCATION IN pluginTerm!!!
    matchListChooser = new window_match_list;
    listWindowUp = FALSE;

    return PLUGIN_OK;
}

// ======================================================================================
static void idaapi pluginTerm( )
{
    //
    // The chooser API is poorly designed. chooser_base_t::call_destructor is called by IDA when the 
    // instance is closed, so if we try to free our allocated struct here, we cause a heap corruption.
    //
    //if (matchListChooser)
    //{
    //    delete matchListChooser;
    //    matchListChooser = NULL;
    //}

    if (iconID != -1)
    {
        free_custom_icon( iconID );
        iconID = -1;
    }

    // Just in case..
    clearMatchData( );
    clearPatternSearchData( );
    freeSignatureData( );
}

// ======================================================================================

// Load signature XML file
static LPSTR xmlValueStr = NULL;
static int   xmlValueBufferSize = 0;
static void XMLCALL characterHandler( PVOID parm, LPCSTR dataStr, int len )
{
    try
    {
        if (xmlValueBufferSize)
        {
            // Increase buffer size as needed
            int adjLen = (len + 1);
            if (xmlValueBufferSize < adjLen)
            {
                if (xmlValueStr = TRealloc<char>( xmlValueStr, adjLen ))
                    xmlValueBufferSize = adjLen;
                else
                {
                    msg( "** Failed to realloc() XML data bufferr! Size wanted: %d **\n", adjLen );
                    xmlValueBufferSize = 0;
                }
            }

            // Save contents
            if (xmlValueBufferSize)
            {
                memcpy( xmlValueStr, dataStr, len );
                xmlValueStr[len] = 0;
            }
        }
    }
    CATCH( );
}
//
ALIGN( 16 ) static char titleStr[1024] = { 0 };
static void XMLCALL startElement( PVOID parm, LPCTSTR nameStr, LPCTSTR *attribStr )
{
    try
    {
        if (xmlValueBufferSize)
        {
            if (*((PWORD)nameStr) == MAKEWORD( 'p', 0 ))
            {
                if (LPCSTR tagPtr = attribStr[0])
                {
                    if (*((PWORD)tagPtr) == MAKEWORD( 't', 0 ))
                    {
                        if (LPCSTR titlePtr = attribStr[1])
                            strncpy( titleStr, titlePtr, SIZESTR( titleStr ) );
                    }
                }
            }
        }

        xmlValueStr[0] = 0;
    }
    CATCH( );
}
//
static void XMLCALL endElement( PVOID parm, LPCSTR name )
{
    try
    {
        if (xmlValueBufferSize)
        {
            if (*((PWORD)name) == MAKEWORD( 'p', 0 ))
            {
                STACKALIGN( sig, SIG );
                sig.title = Heap( ).strdup( titleStr );
                sig.data = NULL;

                if (sig.title)
                {
                    //== Parse data out of the title
                    // Find the last start brace
                    LPSTR stringPtr = titleStr;
                    LPSTR lastBrace = NULL;
                    while (LPSTR pszBrace = strchr( stringPtr, '[' ))
                    {
                        lastBrace = pszBrace;
                        stringPtr = (pszBrace + 1);
                    };

                    if (lastBrace)
                    {
                        // Largest section seen is 16 chars
                        size_t len = strlen( ++lastBrace );
                        lastBrace[len - 1] = 0;

                        // And flag?
                        WORD andFlag = 0;
                        if (lastBrace[len - 2] == '&')
                        {
                            //msg("And: \"%s\"\n", Sig.pszTitle);
                            lastBrace[len - 2] = 0;
                            andFlag = AND;
                        }

                        // First is the optional bits
                        int steps = 0;
                        BOOL endianBail = FALSE;
                        LPSTR bitsStr = lastBrace;
                        if (LPSTR endStr = strchr( lastBrace, '.' ))
                        {
                            *endStr = 0; ++steps;

                            // AND type must have bits
                            sig.bits = 0;
                            if (andFlag)
                            {
                                if (bitsStr[0])
                                {
                                    if (strcmp( bitsStr, "float" ) == 0)
                                        sig.bits = 32;
                                    else
                                        if (strcmp( bitsStr, "double" ) == 0)
                                            sig.bits = 64;
                                        else
                                            sig.bits = atoi( bitsStr );
                                }

                                if (sig.bits == 0)
                                    msg( "** AND type missing bits! \"%s\" **\n", sig.title );
                            }

                            // Next endian and reverse flag
                            // Can be none for default of IDB endian
                            LPSTR endianStr = ++endStr;
                            if (endStr = strchr( endStr, '.' ))
                            {
                                *endStr = 0; ++steps;

                                sig.flags = 0;
                                if (endianStr[0])
                                {
                                    if (*((PWORD)endianStr) == MAKEWORD( 'b', 'e' ))
                                        sig.flags = BIGENDIAN;

                                    // Bail out if altEndianSearch off and opposite our endian
                                    if (!altEndianSearch && ((BYTE)!inf.is_be( ) != (BYTE)sig.flags))
                                    {
                                        //msg("B: \"%s\"\n", sig.title);
                                        endianBail = TRUE;
                                    }
                                    else
                                        if (*((PWORD)(endianStr + 2)) == MAKEWORD( ' ', 'r' ))
                                            sig.flags |= REVERSE;
                                }

                                if (!endianBail)
                                {
                                    sig.flags |= andFlag;

                                    // Last, size
                                    LPSTR sizeStr = (endStr + 1);
                                    sig.size = atoi( sizeStr );
                                    // Valid size required
                                    if ((sig.size > 0) && (sig.size == (strlen( xmlValueStr ) / 2)))
                                    {
                                        ++steps;

                                        // Signature string to bytes
                                        sig.data = (PBYTE)Heap( ).Alloc( sig.size );
                                        if (sig.data)
                                        {
                                            // Hex string to byte data
                                            UINT  size = sig.size;
                                            PBYTE srcPtr = (PBYTE)xmlValueStr;
                                            PBYTE dstPtr = sig.data;

                                            do
                                            {
                                                BYTE hi = (srcPtr[0] - '0');
                                                if (hi > 9) hi -= (('A' - '0') - 10);

                                                BYTE lo = (srcPtr[1] - '0');
                                                if (lo > 9) lo -= (('A' - '0') - 10);

                                                *dstPtr = (lo | (hi << 4));
                                                srcPtr += 2, dstPtr += 1;
                                            } while (--size);

                                            // Save signature
                                            //if(uSize == 0)
                                            {
                                                ++steps;
                                                sigDataBytes += strlen( sig.title );
                                                sigDataBytes += sig.size;
                                                sigList.push_back( sig );
                                            }
                                            //else
                                            //	Heap().Free(Sig.pData);
                                        }
                                    }
                                    else
                                        msg( "** Signature data parse size mismatch! Title: \"%s\" **\n", sig.title );
                                }
                            }
                        }

                        if (steps != 4)
                        {
                            if (!endianBail)
                                msg( "** Failed to parse signature! Title: \"%s\" **\n", sig.title );

                            if (sig.title)
                                Heap( ).Free( sig.title );
                        }
                    }
                    else
                        msg( "** Failed locate info section in title decode! \"%s\" **\n", sig.title );
                }
                else
                {
                    msg( "** Failed to allocate XML title string copy! **\n" );
                    xmlValueBufferSize = 0;
                }
            }
        }

        xmlValueStr[0] = titleStr[0] = 0;
    }
    CATCH( );
}
//
static BOOL loadSignatures( )
{
    BOOL result = FALSE;
    sigDataBytes = 0;

    try
    {
        // Get my module full path replaced with XML file name
        char pathStr[MAX_PATH]; pathStr[0] = pathStr[SIZESTR( pathStr )] = 0;
        GetModuleFileNameEx( GetCurrentProcess( ), myModule, pathStr, SIZESTR( pathStr ) );
        replaceNameInPath( pathStr, SIGFILE );

        FILE *fp = fopen( pathStr, "rb" );
        if (fp)
        {
            long lSize = fsize( fp );
            if (lSize > 0)
            {
                if (LPSTR textStr = TAlloc<char>( lSize + 1 ))
                {
                    // Data value buffer
                    // Largest seen data size 0xFFFF
                    xmlValueBufferSize = 69632;
                    if (xmlValueStr = TAlloc<char>( xmlValueBufferSize ))
                    {
                        textStr[0] = textStr[lSize] = 0;
                        if (fread( textStr, lSize, 1, fp ) == 1)
                        {
                            if (XML_Parser p = XML_ParserCreate( NULL ))
                            {
                                //  7/09/2012 element count: One endian 1,411, both 2278
                                sigList.reserve( 2600 );

                                XML_SetUserData( p, p );
                                XML_SetElementHandler( p, startElement, endElement );
                                XML_SetCharacterDataHandler( p, characterHandler );

                                if (XML_Parse( p, textStr, lSize, 1 ) != XML_STATUS_ERROR)
                                {
                                    result = (xmlValueBufferSize > 0);
                                    sigDataBytes += (sigList.size( ) * sizeof( SIG ));
                                }
                                else
                                    msg( "** Signature XML parse error: \"%s\" at line #%u! **\n", XML_ErrorString( XML_GetErrorCode( p ) ), XML_GetCurrentLineNumber( p ) );

                                XML_ParserFree( p );
                            }
                        }

                        Heap( ).Free( xmlValueStr );
                    }

                    xmlValueBufferSize = 0;
                    Heap( ).Free( textStr );
                }

            }

            fclose( fp );
        }
        else
            msg( "** Signature file \"%s\" not found! **\n", SIGFILE );
    }
    CATCH( );

    return(result);
}

// Free up signature container
static void freeSignatureData( )
{
    if (!sigList.empty( ))
    {
        size_t count = sigList.size( );
        LPSIG e = &sigList[0];
        do
        {
            if (e->title) Heap( ).Free( e->title );
            if (e->data)  Heap( ).Free( e->data );
            e++, --count;
        } while (count);

        sigList.clear( );
    }
}

static void idaapi forumBtnHandler( int button_code, form_actions_t &fa ) { open_url( "http://www.macromonkey.com/bb/index.php/topic,22.0.html" ); }
static void idaapi luigiBtnHandler( int button_code, form_actions_t &fa ) { open_url( "http://aluigi.org/mytoolz.htm#signsrch" ); }

// Process a segment for signatures
extern UINT patternSearch( PBYTE, int, PBYTE, int, int );
static PBYTE pageBuffer = NULL;
static UINT  pageBufferSize = 0;

static void clearProcessSegmentBuffer( )
{
    if (pageBuffer) Heap( ).Free( pageBuffer );
    pageBuffer = NULL;
    pageBufferSize = 0;
}

static void clearMatchData( )
{
    matchList.clear( );
}

static UINT processSegment( segment_t *segPtr )
{
    UINT matches = 0;
    UINT size = (UINT)segPtr->size( );
    if (size)
    {
        if (!pageBuffer)
        {
            // Usually less then 10mb
            pageBufferSize = max( size, (10 * (1024 * 1024)) );
            pageBuffer = TAlloc<BYTE>( pageBufferSize );
            if (!pageBuffer)
            {
                msg( "** Failed to allocate segment bufferr! **\n" );
                pageBufferSize = 0;
                return(0);
            }
        }

        //== Copy IDB bytes to buffer
        // Expand buffer as needed
        if (size > pageBufferSize)
        {
            if (pageBuffer = TRealloc<BYTE>( pageBuffer, size ))
                pageBufferSize = size;
            else
            {
                msg( "** Failed to expand segment buffer! **\n" );
                return(0);
            }
        }

        // Copy speed appears to be constant regardless of what accessor
        // 7-10-2012 About .3 seconds for every 7mb
        // Note: Padded bytes (that don't exist in the source?) will be all 0xFF
        {
            ea_t  currentEa = segPtr->start_ea;
            ea_t  endEa = segPtr->end_ea;
            PBYTE buffer = pageBuffer;
            UINT  count = size;

            do
            {
                *buffer = get_db_byte( currentEa );
                ++currentEa, ++buffer, --count;

            } while (count);

            //DumpData(pPageBuffer, 256);
            //DumpData(pPageBuffer + (uSize - 256), 256);
        }

        // Scan signatures
        {
            // 7-10-2012 about 2 seconds per 6.5mb
            size_t count = sigList.size( );
            LPSIG e = &sigList[0];
            qstring name;
            get_segm_name( &name, segPtr );

            for (size_t i = 0; i < count; i++, e++)
            {
                UINT offset = patternSearch( pageBuffer, size, e->data, e->size, e->bits );
                if (offset != -1)
                {
                    // Get item address points too for code addresses
                    // TOOD: Is there ever data cases too?
                    ea_t address = get_item_head( segPtr->start_ea + offset );
                    //msg("Match %08X \"%s\"\n", eaAddress, e->pszTitle);

                    // Optional output to debug channel
                    if (debugOutput)
                        trace( EAFORMAT" \"%s\"\n", address, e->title );

                    // Optional place comment
                    if (placeComments)
                    {
                        const char prefix[] = { "<$ignsrch> " };
                        qstring comment;

                        // Already has one?
                        ssize_t size = get_cmt( &comment, address, TRUE );
                        if (size > 0)
                        {
                            // Skip if already Signsrch comment
                            if ((size > sizeof( prefix )) && (comment.find( prefix ) != qstring::npos))
                                size = -1;

                            if (size != -1)
                            {
                                // Skip if not enough space
                                if ((size + strlen( e->title ) + sizeof( "\n" )) >= SIZESTR( comment ))
                                    size = -1;

                                if (size != -1)
                                {
                                    // If big add a line break, else just a space
                                    if (size >= 54)
                                    {
                                        comment.append( '\n' );
                                        size += SIZESTR( "\n" );
                                    }
                                    else
                                    {
                                        comment[size] = ' ';
                                        size += SIZESTR( " " );
                                    }
                                }
                            }
                        }
                        else
                            size = 0;

                        if (size >= 0)
                        {
                            comment.append( prefix );
                            comment += '\"';
                            comment.append( e->title );
                            comment += '\"';
                            comment += ' ';
                            set_cmt( address, comment.c_str( ), TRUE );
                        }
                    }

                    MATCH match = { address, i };
                    matchList.push_back( match );
                    matches++;
                }

                if (WaitBox::isUpdateTime( ))
                    if (WaitBox::updateAndCancelCheck( ))
                        return(-1);
            }
        }
    }

    return matches;
}

static bool idaapi pluginRun( size_t arg )
{
    if (!listWindowUp)
    {
        char version[16];
        sprintf( version, "%u.%u", HIBYTE( MY_VERSION ), LOBYTE( MY_VERSION ) );
        msg( "\n>> IDA Signsrch plugin: v: %s, BD: %s, By Sirmabus\n", version, __DATE__ );
        GetModuleHandleEx( (GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS), (LPCTSTR)&pluginRun, &myModule );
        refreshUI( );
        
        if (auto_is_ok( ))
        {
            // Currently we only supports8bit byte processors
            if ((ph.cnbits != 8) || (ph.dnbits != 8))
            {
                msg( "** Sorry only 8bit byte processors are currently supported **\n" );
                msg( "  The processor for this IDB is %d bit code and %d bit data.\n  Please report this issue on the IDA Signsrch support forum.\n", ph.cnbits, ph.dnbits );
                msg( "** Aborted **\n\n" );
                return false;
            }

            // Do main dialog
            altEndianSearch = FALSE;
            debugOutput = FALSE;
            includeCodeSegments = TRUE;
            placeComments = TRUE;
            WORD optionFlags = 0;
            if (altEndianSearch)     optionFlags |= OPT_ALTENDIAN;
            if (debugOutput)		 optionFlags |= OPT_DEBUGOUT;
            if (includeCodeSegments) optionFlags |= OPT_CODESEGS;
            if (placeComments)       optionFlags |= OPT_COMMENTS;

            int uiResult = ask_form( mainForm, version, __DATE__, &optionFlags, forumBtnHandler, luigiBtnHandler );
            if (!uiResult)
            {
                // User canceled, or no options selected, bail out
                msg( " - Canceled -\n" );
                return false;
            }

            altEndianSearch     = ((optionFlags & OPT_ALTENDIAN) != 0);
            debugOutput         = ((optionFlags & OPT_DEBUGOUT) != 0);
            includeCodeSegments = ((optionFlags & OPT_CODESEGS) != 0);
            placeComments       = ((optionFlags & OPT_COMMENTS) != 0);

            WaitBox::show( "Signsrch" );
            WaitBox::updateAndCancelCheck( -1 );
            msg( "IDB: %s endian.\n", (inf.is_be( ) ?  "Big" : "Little") );
            refreshUI( );

            TIMESTAMP startTime = getTimeStamp( );
            if (loadSignatures( ))
            {
                BOOL aborted = FALSE;
                char numBuffer[32];
                msg( "%s signatures loaded, size: %s.\n\n", prettyNumberString( sigList.size( ), numBuffer ), byteSizeString( sigDataBytes ) );
                refreshUI( );

                // Typical matches less then 200, and this is small
                matchList.reserve( 256 );

                if (!sigList.empty( ))
                {
                    totalMatches = 0;

                    // Walk segments
                    int count = get_segm_qty( );
                    for (int i = 0; (i < count) && !aborted; i++)
                    {
                        if (segment_t *seg = getnseg( i ))
                        {
                            //char name[64] = { 0 };
                            qstring name;
                            get_segm_name( &name, seg );
                            
                            qstring classStr;
                            get_segm_class( &classStr, seg );

                            switch (seg->type)
                            {
                            // Types to skip
                            case SEG_XTRN:
                            case SEG_GRP:
                            case SEG_NULL:
                            case SEG_UNDF:
                            case SEG_ABSSYM:
                            case SEG_COMM:
                            case SEG_IMEM:
                            case SEG_CODE:
                            {
                                if (!((seg->type == SEG_CODE) && includeCodeSegments))
                                {
                                    msg( "Skipping segment: \"%s\", \"%s\", %d, " EAFORMAT " - " EAFORMAT ", %s\n", name.c_str( ), classStr.c_str( ), seg->type, seg->start_ea, seg->end_ea, byteSizeString( seg->size( ) ) );
                                    break;
                                }
                            }

                            default:
                            {
                                msg( "Processing segment: \"%s\", \"%s\", %d, " EAFORMAT " - " EAFORMAT ", %s\n", name.c_str( ), classStr.c_str( ), seg->type, seg->start_ea, seg->end_ea, byteSizeString( seg->size( ) ) );
                                UINT matches = processSegment( seg );
                                if (matches > 0)
                                {
                                    if (matches != -1)
                                    {
                                        totalMatches += matches;
                                        msg( "%u matches here.\n", matches );
                                    }
                                    else
                                        aborted = TRUE;
                                }
                            }
                            break;
                            };
                        }
                    }
                    refreshUI( );

                    // Sort match list by address
                    if (!aborted)
                        std::sort( matchList.begin( ), matchList.end( ), MATCH( ) );

                    clearPatternSearchData( );
                    clearProcessSegmentBuffer( );
                }
                else
                {
                    msg( "** No loaded signitures!, Aborted **\n" );
                }    

                if (!aborted)
                {
                    msg( "\nDone: Found %u matches in %s.\n\n", totalMatches, timeString( getTimeStamp( ) - startTime ) );
                    if (debugOutput)
                        trace( "%u signature matches.\n", totalMatches );
                    refreshUI( );

                    if (!matchList.empty( ))
                    {
                        listWindowUp = !matchListChooser->choose( );
                    }
                    else
                    {
                        clearMatchData( );
                        freeSignatureData( );
                    }
                }
                else
                {
                    msg( "** Plugin aborted **\n\n" );
                    clearMatchData( );
                    freeSignatureData( );
                }
            }
            else
            {
                msg( "** Failed to load signitures, Aborted **\n" );
            }        
        }
        else 
        {
            msg( "** Please wait for autoanalysis finish first!, Aborted **\n" );
        }

        refresh_idaview_anyway( );
        WaitBox::hide( );
    }
    else
    {
        PlaySound( (LPCSTR)SND_ALIAS_SYSTEMEXCLAMATION, NULL, (SND_ALIAS_ID | SND_ASYNC) );
    }
    
    return true;
}

