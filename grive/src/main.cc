/*
	grive: an GPL program to sync a local directory with Google Drive
	Copyright (C) 2012  Wan Wai Ho

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License
	as published by the Free Software Foundation version 2
	of the License.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "util/Config.hh"
#include "util/ProgressBar.hh"

#include "base/Drive.hh"
#include "drive2/Syncer2.hh"

#include "http/CurlAgent.hh"
#include "protocol/AuthAgent.hh"
#include "protocol/OAuth2.hh"
#include "json/Val.hh"

#include "bfd/Backtrace.hh"
#include "util/Exception.hh"
#include "util/log/Log.hh"
#include "util/log/CompositeLog.hh"
#include "util/log/DefaultLog.hh"

// boost header
#include <boost/exception/all.hpp>
#include <boost/program_options.hpp>

// initializing libgcrypt, must be done in executable
#include <gcrypt.h>

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <unistd.h>
#include <future>

#include <cpprest/http_listener.h>
#include <cpprest/uri.h>

const std::string default_id            = APP_ID ;
const std::string default_secret        = APP_SECRET ;

using namespace gr ;
namespace po = boost::program_options;

using namespace web::http::experimental::listener;
using namespace web::http;

// libgcrypt insist this to be done in application, not library
void InitGCrypt()
{
	if ( !gcry_check_version(GCRYPT_VERSION) )
		throw std::runtime_error( "libgcrypt version mismatch" ) ;

	// disable secure memory
	gcry_control(GCRYCTL_DISABLE_SECMEM, 0);

	// tell Libgcrypt that initialization has completed
	gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);
}

void InitLog( const po::variables_map& vm )
{
	std::unique_ptr<log::CompositeLog> comp_log( new log::CompositeLog ) ;
	std::unique_ptr<LogBase> def_log( new log::DefaultLog );
	LogBase* console_log = comp_log->Add( def_log ) ;

	if ( vm.count( "log" ) )
	{
		std::unique_ptr<LogBase> file_log( new log::DefaultLog( vm["log"].as<std::string>() ) ) ;
		file_log->Enable( log::debug ) ;
		file_log->Enable( log::verbose ) ;
		file_log->Enable( log::info ) ;
		file_log->Enable( log::warning ) ;
		file_log->Enable( log::error ) ;
		file_log->Enable( log::critical ) ;
		
		// log grive version to log file
		file_log->Log( log::Fmt("grive version " VERSION " " __DATE__ " " __TIME__), log::verbose ) ;
		file_log->Log( log::Fmt("current time: %1%") % DateTime::Now(), log::verbose ) ;
		
		comp_log->Add( file_log ) ;
	}
	
	if ( vm.count( "verbose" ) )
	{
		console_log->Enable( log::verbose ) ;
	}
	
	if ( vm.count( "debug" ) )
	{
		console_log->Enable( log::verbose ) ;
		console_log->Enable( log::debug ) ;
	}
	LogBase::Inst( comp_log.release() ) ;
}

// AuthCode reads an authorization code from the "code" query parameter passed
// via client-side redirect to the redirect uri specified in uri
std::string AuthCode( std::string uri )
{
	// Set up an HTTP listener that is waiting for Google
	// to hit the specified local URI with the authorization
	// code response
	http_listener listener(uri);
	listener.
		open().
		then([uri]() {
			std::cout
				<< "\n"
				<< "Listening on " << uri << " for an authorization code from Google"
				<< std::endl;
		}).
		wait();

	// Set up a promise to read the authorization code from the
	// redirect
	std::promise<std::string> result;
	listener.support(methods::GET, [&result] (http_request req) {
		// Extract the "code" query parameter
		auto params = uri::split_query(req.request_uri().query());
		auto found_code = params.find("code");

		// If auth code is missing, respond with basic web page
		// containing error message
		if (found_code == params.end()) {
			std::cout
				<< "request received without auth code: " << req.absolute_uri().to_string()
				<< std::endl;
			auto msg =
				"grive2 authorization code redirect missing 'code' query parameter.\n\n"
				"Try the auth flow again.";
			req.reply(status_codes::BadRequest, msg);
		}

		// If found, respond with basic web page telling user to close
		// the window and pass the actual code back to the rest of the
		// application
		auto code = found_code->second;
		std::cout << "received authorization code" << std::endl;
		auto msg = "Received grive2 authorization code. You may now close this window.";
		req.reply(status_codes::OK, msg).wait();
		result.set_value(code);
	});

	// Block until we receive an auth code
	std::string code = result.get_future().get();

	// Having received the code, block until listener is shut down
	listener.close().wait();

	return code;
}

int Main( int argc, char **argv )
{
	InitGCrypt() ;
	
	// construct the program options
	po::options_description desc( "Grive options" );
	desc.add_options()
		( "help,h",		"Produce help message" )
		( "version,v",	"Display Grive version" )
		( "auth,a",		"Request authorization token" )
                ( "id,i",               po::value<std::string>(), "Authentication ID")
                ( "secret,e",           po::value<std::string>(), "Authentication secret")
                ( "print-url",          "Only print url for request")
		( "path,p",		po::value<std::string>(), "Path to working copy root")
		( "redirect-uri",	po::value<std::string>(), "local URI on which to listen for auth redirect")
		( "dir,s",		po::value<std::string>(), "Single subdirectory to sync")
		( "verbose,V",	"Verbose mode. Enable more messages than normal.")
		( "log-http",	po::value<std::string>(), "Log all HTTP responses in this file for debugging.")
		( "new-rev",	"Create new revisions in server for updated files.")
		( "debug,d",	"Enable debug level messages. Implies -v.")
		( "log,l",		po::value<std::string>(), "Set log output filename." )
		( "force,f",	"Force grive to always download a file from Google Drive "
						"instead of uploading it." )
		( "upload-only,u", "Do not download anything from Google Drive, only upload local changes" )
		( "no-remote-new,n", "Download only files that are changed in Google Drive and already exist locally" )
		( "dry-run",	"Only detect which files need to be uploaded/downloaded, "
						"without actually performing them." )
		( "upload-speed,U", po::value<unsigned>(), "Limit upload speed in kbytes per second" )
		( "download-speed,D", po::value<unsigned>(), "Limit download speed in kbytes per second" )
		( "progress-bar,P", "Enable progress bar for upload/download of files")
	;
	
	po::variables_map vm;
	try
	{
		po::store( po::parse_command_line( argc, argv, desc ), vm );
	}
	catch( po::error &e )
	{
		std::cerr << "Options are incorrect. Use -h for help\n";
		return -1;
	}
	po::notify( vm );
	
	// simple commands that doesn't require log or config
	if ( vm.count("help") )
	{
		std::cout << desc << std::endl ;
		return 0 ;
	}
	else if ( vm.count( "version" ) )
	{
		std::cout
			<< "grive version " << VERSION << ' ' << __DATE__ << ' ' << __TIME__ << std::endl ;
		return 0 ;
	}

	// initialize logging
	InitLog( vm ) ;
	
	Config config( vm ) ;
	
	Log( "config file name %1%", config.Filename(), log::verbose );

	std::unique_ptr<http::Agent> http( new http::CurlAgent );
	if ( vm.count( "log-http" ) )
		http->SetLog( new http::ResponseLog( vm["log-http"].as<std::string>(), ".txt" ) );

	std::unique_ptr<ProgressBar> pb;
	if ( vm.count( "progress-bar" ) )
	{
		pb.reset( new ProgressBar() );
		http->SetProgressReporter( pb.get() );
	}

	if ( vm.count( "auth" ) )
	{
		std::string id = vm.count( "id" ) > 0
                        ? vm["id"].as<std::string>()
                        : default_id ;
		std::string secret = vm.count( "secret" ) > 0
                        ? vm["secret"].as<std::string>()
                        : default_secret ;
		std::string redirect_uri = config.Get("redirect-uri").Str();

		OAuth2 token( http.get(), id, secret, redirect_uri ) ;
		
		if ( vm.count("print-url") )
		{
			std::cout <<  token.MakeAuthURL() << std::endl ;
			return 0 ;
		}
		
		std::cout
			<< "-----------------------\n"
			<< "Please go to this URL to authorize the app:\n\n"
			<< token.MakeAuthURL()
			<< std::endl ;

		std::string code = AuthCode(redirect_uri);
		token.Auth( code ) ;
		
		// save to config
		config.Set( "id", Val( id ) ) ;
		config.Set( "secret", Val( secret ) ) ;
		config.Set( "refresh_token", Val( token.RefreshToken() ) ) ;
		config.Set( "redirect-uri", Val( redirect_uri ) ) ;
		config.Save() ;
	}
	
	std::string refresh_token ;
	std::string id ;
	std::string secret ;
	std::string redirect_uri ;
	try
	{
		refresh_token = config.Get("refresh_token").Str() ;
		id = config.Get("id").Str() ;
		secret = config.Get("secret").Str() ;
		redirect_uri = config.Get("redirect-uri").Str() ;
	}
	catch ( Exception& e )
	{
		Log(
			"Please run grive with the \"-a\" option if this is the "
			"first time you're accessing your Google Drive!",
			log::critical ) ;
		
		return -1;
	}
	
	OAuth2 token( http.get(), refresh_token, id, secret, redirect_uri ) ;
	AuthAgent agent( token, http.get() ) ;
	v2::Syncer2 syncer( &agent );

	if ( vm.count( "upload-speed" ) > 0 )
		agent.SetUploadSpeed( vm["upload-speed"].as<unsigned>() * 1000 );
	if ( vm.count( "download-speed" ) > 0 )
		agent.SetDownloadSpeed( vm["download-speed"].as<unsigned>() * 1000 );

	Drive drive( &syncer, config.GetAll() ) ;
	drive.DetectChanges() ;

	if ( vm.count( "dry-run" ) == 0 )
	{
		// The progress bar should just be enabled when actual file transfers take place
		if ( pb )
			pb->setShowProgressBar( true ) ;
		drive.Update() ;
		if ( pb )
			pb->setShowProgressBar( false ) ;

		drive.SaveState() ;
	}
	else
		drive.DryRun() ;
		
	config.Save() ;
	Log( "Finished!", log::info ) ;
	return 0 ;
}

int main( int argc, char **argv )
{
	try
	{
		return Main( argc, argv ) ;
	}
	catch ( Exception& e )
	{
		Log( "exception: %1%", boost::diagnostic_information(e), log::critical ) ;
	}
	catch ( std::exception& e )
	{
		Log( "exception: %1%", e.what(), log::critical ) ;
	}
	catch ( ... )
	{
		Log( "unexpected exception", log::critical ) ;
	}
	return -1 ;
}
