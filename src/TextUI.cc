/* 
 *  Ths code in this file is part of tcptrack. For more information see
 *    http://www.rhythm.cx/~steve/devel/tcptrack
 *
 *     Copyright (C) Steve Benson - 2003
 *
 *  tcptrack is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your
 *  option) any later version.
 *   
 *  tcptrack is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *   
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA. 
 *  
 */
#include "TextUI.h"
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <signal.h>
#include <iostream>
#include <fstream>
#include "util.h"
#include "defs.h"
#include "TCPTrack.h"
#include "GenericError.h"

extern TCPTrack *app; 

TextUI::TextUI( TCContainer *c )
{
	container = c;
	iter=NULL;

	doffset=0;

	state=USTATE_IDLE;

	paused=false;
	sort_type=SORT_UN;

	pthread_mutex_init( &state_mutex, NULL );
}

void TextUI::init()
{
	//
	// Initialize ncurses.
	// TODO: make these exceptions more specific & meaningful.
	//

	WINDOW *w=initscr();
	if( w==NULL )
		throw GenericError("Unable to initialize ncurses display: initscr returned NULL.");

	keypad(w,TRUE);
	int x,y;
	getmaxyx(w,y,x); // this is an ncurses macro

	if( x<80 ) 
		throw GenericError("tcptrack requires a screen at least 80 columns wide to run.");

	if( y<3 )
		throw GenericError("tcptrack requires a screen at least 3 rows tall to run.");

	size_x=x;
	size_y=y;
	bottom=y;


	// 
	// Set up and run the displayer thread.
	//

	run_displayer = true;

	pthread_attr_t attr;
	if( pthread_attr_init( &attr ) != 0 )
		throw GenericError("pthread_attr_init() failed");

	pthread_attr_setstacksize( &attr, SS_TUI );

	if( pthread_create(&displayer_tid,&attr,displayer_thread_func,this) != 0 )
		throw GenericError("pthread_create() returned an error.");


	state=USTATE_RUNNING;
}

void TextUI::stop()
{
	pthread_mutex_lock(&state_mutex);
	if( state != USTATE_RUNNING ) 
	{
		pthread_mutex_unlock(&state_mutex);
		return;
	}
	state=USTATE_STOPPING;
	pthread_mutex_unlock(&state_mutex);	

	// now that state is set to USTATE_STOPPING, 
	// the display draw loop will see this and exit. just wait for it.
	pthread_join(displayer_tid,NULL);

	state=USTATE_DONE;

}

TextUI::~TextUI()
{
	stop();
}

// main drawer thread loop
void TextUI::displayer_run()
{
	// used for select(), to grab keyboard input when there is some.
	fd_set fdset;
	struct timeval tv;
	int rv;

	iter=container->getSortedIteratorPtr();

	while( state==USTATE_RUNNING || state==USTATE_IDLE )
	{
		FD_ZERO(&fdset);
		FD_SET(0,&fdset);
		tv.tv_sec=0;
		tv.tv_usec=100;

		rv=select(1,&fdset,NULL,NULL,&tv);
		if( rv )
		{
			int c = getch();
			if( c==KEY_DOWN ) 
			{
				++doffset;
				// this is checked for sanity later
			}
			else if( c==KEY_UP ) 
			{
				if( doffset>0 )
					--doffset;
			}
			else if( c=='q' )
			{
				app->shutdown();
			}
			else if( c=='s' )
			{
				switch( sort_type )
				{
					case SORT_UN:
						sort_type=SORT_RATE;
						break;
					case SORT_RATE:
						sort_type=SORT_BYTES;
						break;
					case SORT_BYTES:
						sort_type=SORT_IDLE;
						break;
					case SORT_IDLE:
						sort_type=SORT_UN;
						break;
				}
			}
			else if( c=='p' )
			{
				if( paused==true ) 
				{
					// going from paused to unpaused
					paused=false;
					container->purge(true);
				}
				else
				{
					// going from unpaused to paused
					paused=true;
					container->purge(false);
				}
			}
		}


		container->lock();

		// check the offset into the container for sanity.
		if( doffset>0 )
		{
			if( container->numConnections()>0 )
			{
				if( doffset >= container->numConnections() ) 
					doffset = container->numConnections()-1;
			}
			else
			{
				doffset=0;
			}
		}

		// if we aren't reusing an old iterator (ie, if unpaused),
		// get a fresh one.		
		if( iter==NULL )
			iter=container->getSortedIteratorPtr();

		drawui();

		if( paused==false )
		{
			// gonna get a new one next time if not paused... 
			// so can this one.
			delete iter;
			iter=NULL;
		}

		container->unlock();


	}
	endwin();
}

void TextUI::drawui()
{
	//write the data to a file also.
	ofstream ipOut;
	ipOut.open ("data.json");

	SortedIterator * i=iter;

	i->rewind();

	int Bps_total=0; // the total speed
	while( TCPConnection *ic=i->getNext() )
		Bps_total+=ic->getPayloadBytesPerSecond();		

	i->rewind();

	if( sort_type != SORT_UN ) 
		i->sort( sort_type );

	ipOut << "{ 'activeConnections' : [ ";

	while( TCPConnection *ic=i->getNext() )
	{
		ipOut << "{";
		ipOut << "'SourceIP' : '" << ic->srcAddr().ptr() << "', 'SourcePort' : '" << ic->srcPort() << "', 'TargetIP' : '" << ic->dstAddr().ptr() << "', 'TargetPort' : '" << ic->dstPort() << "'";
		if( ic->getState() == TCP_STATE_SYN_SYNACK ) {
			ipOut << ", 'ConnectionState': 'SYN_SENT'";
		}
		else if( ic->getState() == TCP_STATE_SYNACK_ACK ) {
			ipOut << ", 'ConnectionState': 'SYNACK_ACK'";
		}
		else if( ic->getState() == TCP_STATE_UP ) {
			ipOut << ", 'ConnectionState': 'ESTABLISHED'";
		}
		else if( ic->getState() == TCP_STATE_FIN_FINACK ) {
			ipOut << ", 'ConnectionState': 'CLOSING'";
		}
		else if( ic->getState() == TCP_STATE_CLOSED ) {
			ipOut << ", 'ConnectionState': 'CLOSED'";
		}
		else if( ic->getState() == TCP_STATE_RESET ) {
			ipOut << ", 'ConnectionState': 'RESET'";
		}

		ipOut << ", 'idleSeconds' : '" << ic->getIdleSeconds() << "'";

		unsigned int Bps = ic->getPayloadBytesPerSecond();

		ipOut << ", 'bps' : '" << Bps << "'},";

	}

	ipOut << " ]";
	ipOut.close();


}

// display the speed with the right format
void TextUI::print_bps(int Bps)
{
	if( Bps < 1024 )
		printw("%d B/s",Bps);
	else if(Bps < 1024*1024 )
		printw("%d KB/s",Bps/1024);
	else if(Bps < 1024*1024*1024 )
		printw("%d MB/s",Bps/(1024*1024));
	else
		printw("%d GB/s",Bps/(1024*1024*1024));
}

// reset the terminal. used only for unclean exits.
void TextUI::reset()
{
	endwin();
}

/////////

void *displayer_thread_func( void *arg )
{
	TextUI *ui = (TextUI *) arg;
	ui->displayer_run();
	return NULL;
}
