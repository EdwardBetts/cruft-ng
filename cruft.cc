#include <iostream>
#include <fstream>
#include <algorithm>
#include <stdio.h>
#include <sys/stat.h>
#include <fnmatch.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>

#include "explain.h"
#include "filters.h"
#include "mlocate.h"
#include "plocate.h"
#include "dpkg.h"
#include "dpkg_exclude.h"

extern int shellexp(char* filename, char* pattern );

using namespace std;

int read_mounts(vector<string>& prunefs, vector<string>& mounts)
{
	// this doesn't include "/", as it always exists
	ifstream mtab("/proc/mounts");
	string mount,type;
	while ( not mtab.eof() )
	{
	      getline(mtab,mount,' '); // discard device
	      getline(mtab,mount,' ');
	      getline(mtab,type,' ');
	      vector<string>::iterator it=prunefs.begin();
	      bool match=false;
		   if (mount=="/") match=true;
	      else if (mount.substr(0,5)=="/sys/") match=true;
	      else if (mount.substr(0,5)=="/dev/") match=true;
	      else for (;it !=prunefs.end();it++) {
		      string uppercase;
		      uppercase=type;
		      transform(uppercase.begin(), uppercase.end(), uppercase.begin(), ::toupper);
		      if (uppercase==*it) {
			  match=true;
			  break;
		      }
	      }
	      if (!match) {
		     //cerr << mount << " => " << type << endl;
		     mounts.push_back(mount);
	      }
	      getline(mtab,mount); // discard options
	}
	return 0;
}

void updatedb(string db)
{
	if (getuid()) return;

	int rc_locate, rc_dpkg;
	struct stat stat_locate, stat_dpkg;
	rc_locate = stat(db.c_str(), &stat_locate);
	rc_dpkg = stat("/var/lib/dpkg/status", &stat_dpkg);

	if (rc_dpkg) {
		cerr << "can't read /var/lib/dpkg/status timestamp !!!" << endl;
		exit(1);
	}

	if (!rc_locate && stat_locate.st_mtim.tv_sec > stat_dpkg.st_mtim.tv_sec)
		return;

	if (system("updatedb")) {
		cerr << "updatedb failed" << endl;
		exit(1);
	}
}

bool myglob(string file, string glob )
{
	bool debug = getenv("DEBUG") != NULL;

	if (file==glob) return true;
	unsigned int filesize=file.size();
	unsigned int globsize=glob.size();
		if ( glob.find("**")==globsize-2
		  and filesize >= globsize-2
		  and file.substr(0,globsize-2)==glob.substr(0,globsize-2)) {
		if (debug) cerr << "match ** " << file << " # " << glob << endl;
		return true;
	}  else if ( glob.find("*")==globsize-1
		  and filesize >= globsize-1
		  and file.find("/",globsize-1)==string::npos
		  and file.substr(0,globsize-1)==glob.substr(0,globsize-1)) {
		if (debug) cerr << "match * " << file << " # " << glob << endl;
		return true;
	} else if ( fnmatch(glob.c_str(),file.c_str(),FNM_PATHNAME)==0 ) {
		if (debug) cerr << "fnmatch " << file << " # " << glob << endl;
		return true;
	} else {
		// fallback to shellexp.c
		char param1[256];
		strncpy(param1,file.c_str(),sizeof(param1));
		param1[sizeof(param1)-1] = '\0';
		char param2[256];
		strncpy(param2,glob.c_str(),sizeof(param2));
		param2[sizeof(param2)-1] = '\0';
		bool result=shellexp(param1,param2);
		if (result and debug) {
			cerr << "shellexp.c " << file << " # " << glob << endl;
		}
		return result;
	}
}

bool pyc_has_py(string pyc)
{
	if (pyc.length() < 15)
		return false;

	// #366616: Don't report .pyc files if a .py file exist

	// also ignore __pycache__ dirs
	// if .py are found in the same directory
	if (pyc.substr(pyc.length()-12, 12) == "/__pycache__") {
		DIR *dp;
		struct dirent *dirp;
		if((dp = opendir(pyc.substr(0, pyc.length()-12).c_str())) == NULL) {
		      return false;
		}
		while ((dirp = readdir(dp)) != NULL) {
			string entry = dirp->d_name;
			if (entry.length() < 4)
				continue;
			if (entry.substr(entry.length()-3,3) == ".py")
				return true;
		}
		return false;
	}

	/* scenario 1:
	/usr/share/python/debpython/debhelper.py
	/usr/share/python/debpython/debhelper.pyc
	*/
	if (pyc.substr(pyc.length()-4, 4) != ".pyc")
		return false;

	struct stat buffer;
	if (stat(pyc.substr(0, pyc.length()-1).c_str(), &buffer) == 0)
		return true;

	/* scenario 2:
	/usr/share/pgcli/pgcli/packages/counter.py
	/usr/share/pgcli/pgcli/packages/__pycache__/counter.cpython-34.pyc
	*/
	size_t pos = pyc.find("/__pycache__/");
	if (pos == string::npos)
		return false;
	pyc.replace(pos, 13, "/");

	pos = pyc.find(".cpython-");
	if (pos == string::npos)
		return false;
	pyc.replace(pos, 15, ".py");

	return stat(pyc.c_str(), &buffer) == 0;
}

void one_file(string infile)
{
	char* file=realpath(infile.c_str(), NULL);
	DIR *dp;
	struct dirent *dirp;
	if((dp = opendir("/usr/lib/cruft/filters-unex/")) == NULL) {
		cerr << "Error(" << errno << ") opening /usr/lib/cruft/filters-unex/" << endl;
		exit(1);
	}
	bool matched = false;
	while ((dirp = readdir(dp)) != NULL) {
		string package=string(dirp->d_name);
		if (package == "." or package == "..") continue;
		ifstream glob_file(("/usr/lib/cruft/filters-unex/" + package).c_str());
		while (glob_file.good())
		{
			string glob_line;
			getline(glob_file,glob_line);
			if (glob_line.substr(0,1) == "/") {
				if (myglob(file,glob_line)) {
					cout << package << endl;
					matched = true;
				}
			}
			if (glob_file.eof()) break;
		}
	}
	if (not matched) cerr << "no matching package found" << endl;
}


int main(int argc, char *argv[])
{
	bool debug = getenv("DEBUG") != NULL;

	if (argc == 2) {
		struct stat buffer;
		if (stat(argv[1], &buffer) == 0) {
			one_file(argv[1]);
			exit(0);
		} else {
			cerr << "file not found" << endl;
			exit(1);
		}
	}

	if (argc > 1) {
		cerr << "cruft-ng [file]" << endl;
		cerr << endl;
		cerr << "if <file> is specified, this file is analysed" << endl;
		cerr << "if not, the whole system is analysed" << endl;
		exit(1);
	}

	std::vector<string> fs,prunefs,mounts;

	struct stat has_plocate;
	if (stat("/var/lib/plocate/plocate.db", &has_plocate) == 0) {
		updatedb("/var/lib/plocate/plocate.db");
		read_plocate(fs,prunefs);
	} else {
		updatedb("/var/lib/mlocate/mlocate.db");
		read_mlocate(fs,prunefs);
	}

	read_mounts(prunefs,mounts);

	const int SIZEBUF = 200;
	char buf[SIZEBUF];
	time_t rawtime;
	struct tm * timeinfo;
	time(&rawtime);
	timeinfo=localtime(&rawtime);
	setlocale(LC_TIME, "");
	strftime(buf, sizeof(buf), "%c", timeinfo);
	cout << "cruft report: " << buf << endl << flush;

	std::vector<string> packages;
	read_dpkg_header(packages);

	std::vector<string> explain;
	read_explain(packages,explain);

	std::vector<string> globs;
	read_filters(packages,globs);

	std::vector<string> dpkg;
	read_dpkg_items(dpkg);

	std::vector<string> excludes;
	read_dpkg_excludes(excludes);

	// match two main data sources
	vector<string> cruft;
	vector<string> missing;
	vector<string>::iterator left=fs.begin();
	vector<string>::iterator right=dpkg.begin();
	while (left != fs.end() && right != dpkg.end() )
	{
		//cerr << "[" << *left << "=" << *right << "]" << endl;
		if (*left==*right) {
			left++;
			right++;
		} else if (*left < *right) {
			cruft.push_back(*left);
			left++;
		} else {
			// file may exist on tmpfs
			// e.g.: /var/cache/apt/archives/partial
			struct stat stat_buffer;
	                if ( stat((*right).c_str(), &stat_buffer)!=0 && *right != "/.") missing.push_back(*right);
			right++;
		}
		if (right == dpkg.end()) while(left  !=fs.end()  ) {cruft.push_back(*left);    left++; };
		if (left  == fs.end()  ) while(right !=dpkg.end()) {missing.push_back(*right); right++;};
	}
	//fs.clear();
	//dpkg.clear();

	if (debug) cerr << missing.size() << " files in missing database" << endl;
	if (debug) cerr << cruft.size() << " files in cruft database" << endl << endl << flush;

	// https://bugs.debian.org/cgi-bin/bugreport.cgi?bug=619086
	vector<string> missing2;
	left=missing.begin();
	while (left != missing.end()) {
		right=excludes.begin();
		bool match=false;
		while (right != excludes.end()) {
			match=myglob(*left,*right);
			if (match) break;
			right++;
		}
		if (!match) missing2.push_back(*left);
		left++;
	}

	// TODO: this should use DPKG database too
	vector<string> cruft2;
	left=cruft.begin();
	right=packages.begin();
	while (left != cruft.end()) {
		if ((*left).substr(0,19) !=  "/var/lib/dpkg/info/") {
			cruft2.push_back(*left);
			left++;
		} else {
			right=packages.begin();
			bool found=false;
			while(right != packages.end() ) {
			    if ( *left=="/var/lib/dpkg/info/" + *right + ".clilibs"
			      or *left=="/var/lib/dpkg/info/" + *right + ".conffiles"
			      or *left=="/var/lib/dpkg/info/" + *right + ".config"
			      or *left=="/var/lib/dpkg/info/" + *right + ".list"
			      or *left=="/var/lib/dpkg/info/" + *right + ".md5sums"
			      or *left=="/var/lib/dpkg/info/" + *right + ".postinst"
			      or *left=="/var/lib/dpkg/info/" + *right + ".postrm"
			      or *left=="/var/lib/dpkg/info/" + *right + ".preinst"
			      or *left=="/var/lib/dpkg/info/" + *right + ".prerm"
			      or *left=="/var/lib/dpkg/info/" + *right + ".shlibs"
			      or *left=="/var/lib/dpkg/info/" + *right + ".symbols"
			      or *left=="/var/lib/dpkg/info/" + *right + ".templates"
			      or *left=="/var/lib/dpkg/info/" + *right + ".triggers")
				{ found=true; break; }
				right++;
			}
			if (!found) cruft2.push_back(*left);
			left++;
		}
	}
	//cruft.clear();
	if (debug) cerr << cruft2.size() << " files in cruft2 database" << endl << endl << std::flush;

	// match the globs against reduced database
	vector<string> cruft3;
	left=cruft2.begin();
	while (left != cruft2.end()) {
		right=globs.begin();
		bool match=false;
		while (right != globs.end()) {
			match=myglob(*left,*right);
			if (match) break;
			right++;
		}
		if (!match) cruft3.push_back(*left);
		left++;
	}

	//cruft2.clear();
	if (debug) cerr << cruft3.size() << " files in cruft3 database" << endl << endl << flush;

	// match the dynamic "explain" filters
	vector<string> cruft4;
	left=cruft3.begin();
	while (left != cruft3.end()) {
		right=explain.begin();
		bool match=false;
		while (right != explain.end()) {
			match=(*left==*right);
			if (match) break;
			right++;
		}
		if (!match) cruft4.push_back(*left);
		left++;
	}

	//cruft3.clear();
	if (debug) cerr << cruft4.size() << " files in cruft4 database" << endl << flush;

	cout << "---- missing: dpkg ----" << endl;
	for (unsigned int i=0;i<missing2.size();i++) {
		cout << "        " << missing2[i] << endl;
	}

	//TODO: split by filesystem
	cout << "---- unexplained: / ----" << endl;
	for (unsigned int i=0;i<cruft4.size();i++) {
		if (!pyc_has_py(cruft4[i]))
			cout << "        " << cruft4[i] << endl;
	}

	// NOT IMPLEMENTED
	cout << "---- broken symlinks: / ----" << endl;
	cout << endl << "end." << endl;
	return 0;
}
