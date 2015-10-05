/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2015 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include <osgEarth/ShaderLoader>
#include <osgEarth/URI>
#include <osgEarth/VirtualProgram>
#include <osgDB/FileUtils>

#undef  LC
#define LC "[ShaderLoader] "

using namespace osgEarth;


typedef std::map<std::string,std::string> StringMap;


// find the value of a quoted pragma, e.g.:
//   #pragma oe_key "value"
// returns the string "value" (without the quotes).
std::string
ShaderLoader::getQuotedPragmaValue(const std::string& source, const std::string& key)
{
    std::string::size_type includePos = source.find("#pragma " + key);
    if ( includePos == std::string::npos )
        return "";

    std::string::size_type openQuotePos = source.find('\"', includePos);
    if ( openQuotePos == std::string::npos )
        return "";

    std::string::size_type closeQuotePos = source.find('\"', openQuotePos+1);
    if ( closeQuotePos == std::string::npos )
        return "";

    std::string statement = source.substr( includePos, (closeQuotePos-includePos)+1 );

    return source.substr( openQuotePos+1, (closeQuotePos-openQuotePos)-1 );
}

void
ShaderLoader::getAllQuotedPragmaValues(const std::string&     source,
                                       const std::string&     key,
                                       std::set<std::string>& output)
{
    std::string::size_type includePos = 0;
    while( includePos != std::string::npos )
    {
        includePos = source.find("#pragma " + key, includePos);
        if ( includePos != std::string::npos )
        {
            std::string::size_type openQuotePos = source.find('\"', includePos);
            if ( openQuotePos != std::string::npos )
            {
                std::string::size_type closeQuotePos = source.find('\"', openQuotePos+1);
                if ( closeQuotePos != std::string::npos )
                {
                    const size_t len = (closeQuotePos - openQuotePos) - 1;
                    if ( len )
                        output.insert( source.substr( openQuotePos+1, len ) );

                    includePos = closeQuotePos;
                }
            }
        }
    }
}

std::string
ShaderLoader::load(const std::string&    filename,
                   const ShaderPackage&  package,
                   const osgDB::Options* dbOptions)
{
    std::string output;
    bool useInlineSource = false;

    URIContext context( dbOptions );
    URI uri(filename, context);

    std::string inlineSource;
    ShaderPackage::SourceMap::const_iterator source = package._sources.find(filename);
    if ( source != package._sources.end() )
        inlineSource = source->second;

    std::string path = osgDB::findDataFile(uri.full(), dbOptions);
    if ( path.empty() )
    {
        output = inlineSource;
        useInlineSource = true;
        if ( inlineSource.empty() )
        {
            OE_WARN << LC << "Inline source for \"" << filename << "\" is empty, and no external file could be found.\n";
        }
    }
    else
    {
        std::string externalSource = URI(path, context).getString(dbOptions);
        if (!externalSource.empty())
        {
            OE_DEBUG << LC << "Loaded external shader " << filename << " from " << path << "\n";
            output = externalSource;
        }
        else
        {
            output = inlineSource;
            useInlineSource = true;
        }
    }

    // replace common tokens:
    osgEarth::replaceIn(output, "$GLSL_VERSION_STR", GLSL_VERSION_STR);
    osgEarth::replaceIn(output, "$GLSL_DEFAULT_PRECISION_FLOAT", GLSL_DEFAULT_PRECISION_FLOAT);

    // If we're using inline source, we have to post-process the string.
    if ( useInlineSource )
    {
        // reinstate preprocessor macros since GCC doesn't like them in the inlines shaders.
        // The token was inserted in the CMakeModules/ConfigureShaders.cmake.in script.
        osgEarth::replaceIn(output, "$__HASHTAG__", "#");
    }

    // Process any "#pragma include" statements
    while(true)
    {
        std::string::size_type includePos = output.find("#pragma include");
        if ( includePos == std::string::npos )
            break;

        std::string::size_type openQuotePos = output.find('\"', includePos);
        if ( openQuotePos == std::string::npos )
            break;

        std::string::size_type closeQuotePos = output.find('\"', openQuotePos+1);
        if ( closeQuotePos == std::string::npos )
            break;

        std::string includeStatement = output.substr( includePos, (closeQuotePos-includePos)+1 );

        std::string fileToInclude = output.substr( openQuotePos+1, (closeQuotePos-openQuotePos)-1 );

        // load the source of the included file, and append a newline so we
        // don't break the MULTILINE macro if the last line of the include
        // file is a comment.
        std::string fileSource = Stringify()
            << load(fileToInclude, package, dbOptions)
            << "\n";

        osgEarth::replaceIn(output, includeStatement, fileSource);
    }

    // Process any "#pragma define" statements
    while(true)
    {
        std::string::size_type definePos = output.find("#pragma vp_define");
        if ( definePos == std::string::npos )
            break;

        std::string::size_type openQuotePos = output.find('\"', definePos);
        if ( openQuotePos == std::string::npos )
            break;

        std::string::size_type closeQuotePos = output.find('\"', openQuotePos+1);
        if ( closeQuotePos == std::string::npos )
            break;

        std::string defineStatement = output.substr( definePos, (closeQuotePos-definePos)+1 );

        std::string varName = output.substr( openQuotePos+1, (closeQuotePos-openQuotePos)-1 );

        ShaderPackage::DefineMap::const_iterator d = package._defines.find( varName );

        bool defineIt =
            d != package._defines.end() &&
            d->second == true;

        std::string newStatement = Stringify()
            << (defineIt? "#define " : "#undef ")
            << varName;

        osgEarth::replaceIn( output, defineStatement, newStatement );
    }

    // Finally, process any replacements.
    for(ShaderPackage::ReplaceMap::const_iterator i = package._replaces.begin();
        i != package._replaces.end();
        ++i)
    {
        osgEarth::replaceIn( output, i->first, i->second );
    }

    return output;
}

std::string
ShaderLoader::load(const std::string&    filename,
                   const std::string&    inlineSource,
                   const osgDB::Options* dbOptions )
{
    std::string output;
    bool useInlineSource = false;

    URIContext context( dbOptions );
    URI uri(filename, context );

    std::string path = osgDB::findDataFile(filename, dbOptions);
    if ( path.empty() )
    {
        output = inlineSource;
        useInlineSource = true;
    }
    else
    {
        std::string externalSource = URI(path, context).getString(dbOptions);
        if (!externalSource.empty())
        {
            OE_DEBUG << LC << "Loaded external shader " << filename << " from " << path << "\n";
            output = externalSource;
        }
        else
        {
            output = inlineSource;
            useInlineSource = true;
        }
    }

    // replace common tokens:
    osgEarth::replaceIn(output, "$GLSL_VERSION_STR", GLSL_VERSION_STR);
    osgEarth::replaceIn(output, "$GLSL_DEFAULT_PRECISION_FLOAT", GLSL_DEFAULT_PRECISION_FLOAT);

    // If we're using inline source, we have to post-process the string.
    if ( useInlineSource )
    {
        // reinstate preprocessor macros since GCC doesn't like them in the inlines shaders.
        // The token was inserted in the CMakeModules/ConfigureShaders.cmake.in script.
        osgEarth::replaceIn(output, "$__HASHTAG__", "#");
    }

    return output;
}

bool
ShaderLoader::load(VirtualProgram*       vp,
                   const std::string&    filename,
                   const ShaderPackage&  package,
                   const osgDB::Options* dbOptions)
{
    if ( !vp )
    {
        OE_WARN << LC << "Illegal: null VirtualProgram\n";
        return false;
    }

    std::string source = load(filename, package, dbOptions);
    if ( source.empty() )
    {
        OE_WARN << LC << "Failed to load shader source from \"" << filename << "\"\n";
        return false;
    }

    std::string loc = getQuotedPragmaValue(source, "vp_location");
    ShaderComp::FunctionLocation location;
    bool locationSet = true;

    if      ( ciEquals(loc, "vertex_model") )
        location = ShaderComp::LOCATION_VERTEX_MODEL;
    else if ( ciEquals(loc, "vertex_view") )
        location = ShaderComp::LOCATION_VERTEX_VIEW;
    else if ( ciEquals(loc, "vertex_clip") )
        location = ShaderComp::LOCATION_VERTEX_CLIP;
    else if ( ciEquals(loc, "tess_control") || ciEquals(loc, "tessellation_control") )
        location = ShaderComp::LOCATION_TESS_CONTROL;
    else if ( ciEquals(loc, "tess_eval") || ciEquals(loc, "tessellation_eval") || ciEquals(loc, "tessellation_evaluation") || ciEquals(loc, "tess_evaluation") )
        location = ShaderComp::LOCATION_TESS_EVALUATION;
    else if ( ciEquals(loc, "vertex_geometry") || ciEquals(loc, "geometry") )
        location = ShaderComp::LOCATION_GEOMETRY;
    else if ( ciEquals(loc, "fragment" ) )
        location = ShaderComp::LOCATION_FRAGMENT_COLORING;
    else if ( ciEquals(loc, "fragment_coloring") )
        location = ShaderComp::LOCATION_FRAGMENT_COLORING;
    else if ( ciEquals(loc, "fragment_lighting") )
        location = ShaderComp::LOCATION_FRAGMENT_LIGHTING;
    else if ( ciEquals(loc, "fragment_output") )
        location = ShaderComp::LOCATION_FRAGMENT_OUTPUT;
    else
    {
        locationSet = false;
    }

    // If entry point is set, this is a function; otherwise a simple library.
    std::string entryPoint = getQuotedPragmaValue(source, "vp_entryPoint");

    // order is optional.
    std::string orderStr = getQuotedPragmaValue(source, "vp_order");


    // Now that we've read all of our #pragmas, remove the quotation marks from the source since they are illegal in GLSL
    replaceIn( source, "\"", " ");        

    if ( !entryPoint.empty() )
    {
        if ( !locationSet )
        {
            OE_WARN << LC << "Illegal: shader \"" << filename << "\" has invalid #pragma vp_location when vp_entryPoint is set\n";
            return false;
        }

        float order;
        if ( ciEquals(orderStr, "FLT_MAX") || ciEquals(orderStr, "last") )
            order = FLT_MAX;
        else if ( ciEquals(orderStr, "-FLT_MAX") || ciEquals(orderStr, "first") )
            order = -FLT_MAX;
        else
            order = as<float>(orderStr, 1.0f);

        // set the function!
        vp->setFunction( entryPoint, source, location, 0L, order );
    }

    else
    {
        // Replace all quotation marks with spaces since they are illegal in GLSL
        replaceIn( source, "\"", " ");        
 
        // install as a simple shader.
        if ( locationSet )
        {
            // If a location is set, install in that location only
            osg::Shader::Type type =
                location == ShaderComp::LOCATION_VERTEX_MODEL || location == ShaderComp::LOCATION_VERTEX_VIEW || location == ShaderComp::LOCATION_VERTEX_CLIP ? osg::Shader::VERTEX :
                osg::Shader::FRAGMENT;

            osg::Shader* shader = new osg::Shader(type, source);
            shader->setName( filename );
            vp->setShader( filename, shader );
        }

        else
        {
            // If no location was set, install in all stages.
            osg::Shader::Type types[5] = { osg::Shader::VERTEX, osg::Shader::FRAGMENT, osg::Shader::GEOMETRY, osg::Shader::TESSCONTROL, osg::Shader::TESSEVALUATION };
            for(int i=0; i<5; ++i)
            {
                osg::Shader* shader = new osg::Shader(types[i], source);
                std::string name = Stringify() << filename + "_" + shader->getTypename();
                shader->setName( name );
                vp->setShader( name, shader );
            }
        }
    }

    return true;
}

bool
ShaderLoader::unload(VirtualProgram*       vp,
                     const std::string&    filename,
                     const ShaderPackage&  package,
                     const osgDB::Options* dbOptions)
{
    if ( !vp )
    {
        // fail quietly
        return false;
    }

    std::string source = load(filename, package, dbOptions);
    if ( source.empty() )
    {
        OE_WARN << LC << "Failed to unload shader source from \"" << filename << "\"\n";
        return false;
    }

    std::string entryPoint = getQuotedPragmaValue(source, "vp_entryPoint");
    if ( !entryPoint.empty() )
    {
        vp->removeShader( entryPoint );
    }
    else
    {
        vp->removeShader( filename );
    }
    return true;
}

//...................................................................

void
ShaderPackage::define(const std::string& name,
                      bool               defOrUndef)
{
    _defines[name] = defOrUndef;
}

void
ShaderPackage::replace(const std::string& pattern,
                       const std::string& value)
{
    _replaces[pattern] = value;
}

bool
ShaderPackage::load(VirtualProgram*       vp,
                    const std::string&    filename,
                    const osgDB::Options* dbOptions) const
{
    return ShaderLoader::load(vp, filename, *this, dbOptions);
}

bool
ShaderPackage::unload(VirtualProgram*       vp,
                      const std::string&    filename,
                      const osgDB::Options* dbOptions) const
{
    return ShaderLoader::unload(vp, filename, *this, dbOptions);
}

bool
ShaderPackage::loadAll(VirtualProgram*       vp,
                       const osgDB::Options* dbOptions) const
{
    int oks = 0;
    for(SourceMap::const_iterator i = _sources.begin(); i != _sources.end(); ++i)
    {
        oks += load( vp, i->first ) ? 1 : 0;
    }
    return oks == _sources.size();
}

bool
ShaderPackage::unloadAll(VirtualProgram*       vp,
                          const osgDB::Options* dbOptions) const
{
    int oks = 0;
    for(SourceMap::const_iterator i = _sources.begin(); i != _sources.end(); ++i)
    {
        oks += unload( vp, i->first ) ? 1 : 0;
    }
    return oks == _sources.size();
}