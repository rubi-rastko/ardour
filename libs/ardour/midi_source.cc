/*
 * Copyright (C) 2006-2016 David Robillard <d@drobilla.net>
 * Copyright (C) 2007-2018 Paul Davis <paul@linuxaudiosystems.com>
 * Copyright (C) 2008-2009 Hans Baier <hansfbaier@googlemail.com>
 * Copyright (C) 2009-2012 Carl Hetherington <carl@carlh.net>
 * Copyright (C) 2012-2016 Tim Mayberry <mojofunk@gmail.com>
 * Copyright (C) 2015-2019 Robin Gareus <robin@gareus.org>
 * Copyright (C) 2016 Nick Mainsbridge <mainsbridge@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <float.h>
#include <cerrno>
#include <ctime>
#include <cmath>
#include <iomanip>
#include <algorithm>

#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>

#include "pbd/xml++.h"
#include "pbd/pthread_utils.h"
#include "pbd/basename.h"
#include "pbd/timing.h"

#include "evoral/Control.h"
#include "evoral/EventSink.h"

#include "ardour/debug.h"
#include "ardour/file_source.h"
#include "ardour/midi_channel_filter.h"
#include "ardour/midi_cursor.h"
#include "ardour/midi_model.h"
#include "ardour/midi_source.h"
#include "ardour/midi_state_tracker.h"
#include "ardour/session.h"
#include "ardour/session_directory.h"
#include "ardour/source_factory.h"
#include "ardour/tempo.h"
#include "ardour/evoral_types_convert.h"
#include "ardour/types_convert.h"

#include "pbd/i18n.h"

namespace ARDOUR { template <typename T> class MidiRingBuffer; }

using namespace std;
using namespace ARDOUR;
using namespace PBD;

MidiSource::MidiSource (Session& s, string name, Source::Flag flags)
	: Source(s, DataType::MIDI, name, flags)
	, _writing(false)
	, _capture_length(0)
{
}

MidiSource::MidiSource (Session& s, const XMLNode& node)
	: Source(s, node)
	, _writing(false)
	, _capture_length(0)
{
	if (set_state (node, Stateful::loading_state_version)) {
		throw failed_constructor();
	}
}

MidiSource::~MidiSource ()
{
	/* invalidate any existing iterators */
	Invalidated (false);
}

XMLNode&
MidiSource::get_state () const
{
	XMLNode& node (Source::get_state());

	if (_captured_for.length()) {
		node.set_property ("captured-for", _captured_for);
	}

	for (InterpolationStyleMap::const_iterator i = _interpolation_style.begin(); i != _interpolation_style.end(); ++i) {
		XMLNode* child = node.add_child (X_("InterpolationStyle"));
		child->set_property (X_("parameter"), EventTypeMap::instance().to_symbol (i->first));
		child->set_property (X_("style"), i->second);
	}

	for (AutomationStateMap::const_iterator i = _automation_state.begin(); i != _automation_state.end(); ++i) {
		XMLNode* child = node.add_child (X_("AutomationState"));
		child->set_property (X_("parameter"), EventTypeMap::instance().to_symbol (i->first));
		child->set_property (X_("state"), i->second);
	}

	return node;
}

int
MidiSource::set_state (const XMLNode& node, int /*version*/)
{
	node.get_property ("captured-for", _captured_for);

	std::string str;
	XMLNodeList children = node.children ();
	for (XMLNodeConstIterator i = children.begin(); i != children.end(); ++i) {
		if ((*i)->name() == X_("InterpolationStyle")) {
			if (!(*i)->get_property (X_("parameter"), str)) {
				error << _("Missing parameter property on InterpolationStyle") << endmsg;
				return -1;
			}
			Evoral::Parameter p = EventTypeMap::instance().from_symbol (str);

			switch (p.type()) {
			case MidiCCAutomation:
			case MidiPgmChangeAutomation:       break;
			case MidiChannelPressureAutomation: break;
			case MidiNotePressureAutomation:    break;
			case MidiPitchBenderAutomation:     break;
			case MidiSystemExclusiveAutomation:
				cerr << "Parameter \"" << str << "\" is system exclusive - no automation possible!\n";
				continue;
			default:
				cerr << "Parameter \"" << str << "\" found for MIDI source ... not legal; ignoring this parameter\n";
				continue;
			}

			/* backwards compat, older versions (<= 7000) saved an empty string for non default */
			if ((*i)->get_property (X_("style"), str)) {
				if (str.empty ()) {
					set_interpolation_of (p, EventTypeMap::instance().interpolation_of (p) == AutomationList::Discrete ? AutomationList::Linear : AutomationList::Discrete);
					continue;
				}
			}
			AutomationList::InterpolationStyle s;
			if (!(*i)->get_property (X_("style"), s)) {
				error << _("Missing style property on InterpolationStyle") << endmsg;
				return -1;
			}
			set_interpolation_of (p, s);

		} else if ((*i)->name() == X_("AutomationState")) {
			if (!(*i)->get_property (X_("parameter"), str)) {
				error << _("Missing parameter property on AutomationState") << endmsg;
				return -1;
			}
			Evoral::Parameter p = EventTypeMap::instance().from_symbol (str);

			/* backwards compat, older versions (<= 7000) saved an empty string for "off" */
			if ((*i)->get_property (X_("state"), str)) {
				if (str.empty ()) {
					set_automation_state_of (p, Off);
					continue;
				}
			}

			AutoState s;
			if (!(*i)->get_property (X_("state"), s)) {
				error << _("Missing state property on AutomationState") << endmsg;
				return -1;
			}
			set_automation_state_of (p, s);
		}
	}

	return 0;
}

void
MidiSource::invalidate (const WriterLock& lock)
{
	Invalidated(_session.transport_rolling());
}

timecnt_t
MidiSource::midi_read (const ReaderLock&                  lm,
                       Evoral::EventSink<samplepos_t>&    dst,
                       timepos_t const &                  source_start,
                       timepos_t const &                  start,
                       timecnt_t const &                  cnt,
                       Temporal::Range*                   loop_range,
                       MidiCursor&                        cursor,
                       MidiNoteTracker*                   tracker,
                       MidiChannelFilter*                 filter,
                       const std::set<Evoral::Parameter>& filtered)
{
	Timing t;

	DEBUG_TRACE (DEBUG::MidiSourceIO,
	             string_compose ("MidiSource::midi_read() %5 sstart %1 start %2 cnt %3 tracker %4\n",
	                             source_start, start, cnt, tracker, name()));

	if (!_model) {
		return timecnt_t (read_unlocked (lm, dst, source_start, start, cnt, loop_range, tracker, filter), start);
	}

	// Find appropriate model iterator

	const bool linear_read = cursor.last_read_end != 0 && start == cursor.last_read_end;
	if (!linear_read || !cursor.iter.valid()) {
		/* Cached iterator is invalid, search for the first event past start.
		   Note that multiple tracks can use a MidiSource simultaneously, so
		   all playback state must be in parameters (the cursor) and must not
		   be cached in the source of model itself.
		   See https://tracker.ardour.org/view.php?id=6541
		*/
		cursor.connect(Invalidated);
		cursor.iter = _model->begin (start.beats(), false, filtered, &cursor.active_notes);
		cursor.active_notes.clear();
	}

	cursor.last_read_end = start + cnt;

	// Find appropriate model iterator
	Evoral::Sequence<Temporal::Beats>::const_iterator& i = cursor.iter;

	// Copy events in [start, start + cnt) into dst

	const Temporal::Beats source_start_beats = source_start.beats();
	const Temporal::Beats region_start_beats = start.beats();
	const Temporal::Beats cnt_beats = cnt.beats ();

	const Temporal::Beats end = source_start_beats + region_start_beats + cnt_beats;
	const Temporal::Beats session_source_start = (source_start + start).beats();

	for (; i != _model->end(); ++i) {

		// Offset by source start to convert event time to session time

		const Temporal::Beats session_event_beats = source_start_beats + i->time();

		if (session_event_beats < session_source_start) {
			/* event too early */
			DEBUG_TRACE (DEBUG::MidiSourceIO, string_compose ("%1: skip event, too early @ %2 for %3\n", _name, session_event_beats, session_source_start));
			continue;

		} else if (session_event_beats >= end) {

			DEBUG_TRACE (DEBUG::MidiSourceIO, string_compose ("%1: reached end (%2) with event @ %3\n", _name, end, session_event_beats));
			break;

		} else {

			/* in range */

			timepos_t seb = timepos_t (session_event_beats);
			samplepos_t time_samples = seb.samples();

			if (loop_range) {
				time_samples = loop_range->squish (seb).samples();
			}

			const uint8_t status           = i->buffer()[0];
			const bool    is_channel_event = (0x80 <= (status & 0xF0)) && (status <= 0xE0);

			if (filter && is_channel_event) {
				/* Copy event so the filter can modify the channel.  I'm not
				 * sure if this is necessary here (channels are mapped later in
				 * buffers anyway), but it preserves existing behaviour without
				 *  destroying events in the model during read.
				 */
				Evoral::Event<Temporal::Beats> ev(*i, true);

				if (!filter->filter(ev.buffer(), ev.size())) {
					dst.write (time_samples, ev.event_type(), ev.size(), ev.buffer());
				} else {
					DEBUG_TRACE (DEBUG::MidiSourceIO, string_compose ("%1: filter event @ %2 type %3 size %4\n", _name, time_samples, i->event_type(), i->size()));
				}
			} else {
				dst.write (time_samples, i->event_type(), i->size(), i->buffer());
			}


#ifndef NDEBUG
			if (DEBUG_ENABLED(DEBUG::MidiSourceIO)) {
				DEBUG_STR_DECL(a);
				DEBUG_STR_APPEND(a, string_compose ("%1 added event @ %2 (%3) sz %4 within %5 .. %6 ", _name, time_samples, session_event_beats, i->size(), source_start + start, end));
				for (size_t n=0; n < i->size(); ++n) {
					DEBUG_STR_APPEND(a,hex);
					DEBUG_STR_APPEND(a,"0x");
					DEBUG_STR_APPEND(a,(int)i->buffer()[n]);
					DEBUG_STR_APPEND(a,' ');
				}
				DEBUG_STR_APPEND(a,'\n');
				DEBUG_TRACE (DEBUG::MidiSourceIO, DEBUG_STR(a).str());
			}
#endif

			if (tracker) {
				tracker->track (*i);
			}
		}
	}
	t.update ();

	return cnt;
}

timecnt_t
MidiSource::midi_write (const WriterLock&            lm,
                        MidiRingBuffer<samplepos_t>& source,
                        timepos_t const &            source_start,
                        timecnt_t const &            cnt)
{
	const timecnt_t ret = write_unlocked (lm, source, source_start, cnt);

	if (cnt == timecnt_t::max (cnt.time_domain())) {
		invalidate(lm);
	} else {
		_capture_length += cnt.samples();
	}

	return ret;
}

void
MidiSource::mark_streaming_midi_write_started (const WriterLock& lock, NoteMode mode)
{
	if (_model) {
		/* XXX do something with note mode?  */
		_model->start_write ();
	}

	_writing = true;
}

void
MidiSource::mark_write_starting_now (timepos_t const & position, samplecnt_t capture_length)
{
	/* I'm not sure if this is the best way to approach this, but
	   _capture_length needs to be set up with the transport sample
	   when a record actually starts, as it is used by
	   SMFSource::write_unlocked to decide whether incoming notes
	   are within the correct time range.
	   mark_streaming_midi_write_started (perhaps a more logical
	   place to do this) is not called at exactly the time when
	   record starts, and I don't think it necessarily can be
	   because it is not RT-safe.
	*/

	set_natural_position (position);

	_capture_length      = capture_length;

	/* currently prefer to compute length in beats, since that matches 6.x
	 * and earlier behavior
	 */

	timecnt_t distance = timecnt_t (timepos_t (capture_length), timepos_t (position));
	_length = timecnt_t (distance.beats(), timepos_t (position));
}

void
MidiSource::mark_streaming_write_started (const WriterLock& lock)
{
	/* as of March 2022 or long before , the note mode argument does nothing */
	mark_streaming_midi_write_started (lock, Sustained);
}

void
MidiSource::mark_midi_streaming_write_completed (const WriterLock&                                  lock,
                                                 Evoral::Sequence<Temporal::Beats>::StuckNoteOption option,
                                                 Temporal::timecnt_t const &                        duration)
{
	if (_model) {
		_model->end_write (option, duration.beats());

		/* Make captured controls discrete to play back user input exactly. */
		for (MidiModel::Controls::iterator i = _model->controls().begin(); i != _model->controls().end(); ++i) {
			if (i->second->list()) {
				i->second->list()->set_interpolation (AutomationList::Discrete);
				_interpolation_style.insert(std::make_pair(i->second->parameter(), AutomationList::Discrete));
			}
		}
	}

	invalidate(lock);
	_writing = false;
}

void
MidiSource::mark_streaming_write_completed (const WriterLock& lock, Temporal::timecnt_t const & duration)
{
	mark_midi_streaming_write_completed (lock, Evoral::Sequence<Temporal::Beats>::DeleteStuckNotes, duration);
}

int
MidiSource::export_write_to (const ReaderLock& lock, std::shared_ptr<MidiSource> newsrc, Temporal::Beats begin, Temporal::Beats end)
{
	WriterLock newsrc_lock (newsrc->mutex ());

	if (!_model) {
		error << string_compose (_("programming error: %1"), X_("no model for MidiSource during export"));
		return -1;
	}

	_model->write_section_to (newsrc, newsrc_lock, begin, end, true);

	newsrc->flush_midi(newsrc_lock);

	return 0;
}

int
MidiSource::write_to (const ReaderLock& lock, std::shared_ptr<MidiSource> newsrc, Temporal::Beats begin, Temporal::Beats end)
{
	WriterLock newsrc_lock (newsrc->mutex ());

	newsrc->set_natural_position (_natural_position);
	newsrc->copy_interpolation_from (this);
	newsrc->copy_automation_state_from (this);

	if (_model) {
		if (begin == Temporal::Beats() && end == std::numeric_limits<Temporal::Beats>::max()) {
			_model->write_to (newsrc, newsrc_lock);
		} else {
			_model->write_section_to (newsrc, newsrc_lock, begin, end);
		}
	} else {
		error << string_compose (_("programming error: %1"), X_("no model for MidiSource during ::clone()"));
		return -1;
	}

	newsrc->flush_midi(newsrc_lock);


	if (begin != Temporal::Beats() || end != std::numeric_limits<Temporal::Beats>::max()) {
		/* force a reload of the model if the range is partial */
		newsrc->load_model (newsrc_lock, true);
	} else {
		/* re-create model */
		newsrc->destroy_model (newsrc_lock);
		newsrc->load_model (newsrc_lock);
	}

	/* this file is not removable (but since it is MIDI, it is mutable) */

	std::dynamic_pointer_cast<FileSource> (newsrc)->prevent_deletion ();

	return 0;
}

void
MidiSource::session_saved()
{
	WriterLock lm (_lock);

	/* this writes a copy of the data to disk.
	   XXX do we need to do this every time?
	*/

	if (_model && _model->edited()) {
		/* The model is edited, write its contents into the current source
		   file (overwiting previous contents). */

		/* Temporarily drop our reference to the model so that as the model
		   pushes its current state to us, we don't try to update it. */
		std::shared_ptr<MidiModel> mm = _model;
		_model.reset ();

		/* Flush model contents to disk. */
		mm->sync_to_source (lm);

		/* Reacquire model. */
		_model = mm;

	} else {
		flush_midi(lm);
	}
}

void
MidiSource::drop_model (const WriterLock& lock)
{
	_model.reset();
	invalidate(lock);
	ModelChanged (); /* EMIT SIGNAL */
}

void
MidiSource::set_model (const WriterLock& lock, std::shared_ptr<MidiModel> m)
{
	_model = m;
	std::cerr << "Source " << name() << " switched to model " << _model << std::endl;
	invalidate(lock);
	ModelChanged (); /* EMIT SIGNAL */
}

AutomationList::InterpolationStyle
MidiSource::interpolation_of (Evoral::Parameter const& p) const
{
	InterpolationStyleMap::const_iterator i = _interpolation_style.find (p);
	if (i == _interpolation_style.end()) {
		return EventTypeMap::instance().interpolation_of (p);
	}

	return i->second;
}

AutoState
MidiSource::automation_state_of (Evoral::Parameter const& p) const
{
	AutomationStateMap::const_iterator i = _automation_state.find (p);
	if (i == _automation_state.end()) {
		/* default to `play', otherwise if MIDI is recorded /
		   imported with controllers etc. they are by default
		   not played back, which is a little surprising.
		*/
		return Play;
	}

	return i->second;
}

/** Set interpolation style to be used for a given parameter.  This change will be
 *  propagated to anyone who needs to know.
 */
void
MidiSource::set_interpolation_of (Evoral::Parameter const& p, AutomationList::InterpolationStyle s)
{
	if (interpolation_of (p) == s) {
		return;
	}

	if (EventTypeMap::instance().interpolation_of (p) == s) {
		/* interpolation type is being set to the default, so we don't need a note in our map */
		_interpolation_style.erase (p);
	} else {
		_interpolation_style[p] = s;
	}

	InterpolationChanged (p, s); /* EMIT SIGNAL */
}

void
MidiSource::set_automation_state_of (Evoral::Parameter const& p, AutoState s)
{
	if (automation_state_of (p) == s) {
		return;
	}

	if (s == Play) {
		/* automation state is being set to the default, so we don't need a note in our map */
		_automation_state.erase (p);
	} else {
		_automation_state[p] = s;
	}

	AutomationStateChanged (p, s); /* EMIT SIGNAL */
}

void
MidiSource::copy_interpolation_from (std::shared_ptr<MidiSource> s)
{
	copy_interpolation_from (s.get ());
}

void
MidiSource::copy_automation_state_from (std::shared_ptr<MidiSource> s)
{
	copy_automation_state_from (s.get ());
}

void
MidiSource::copy_interpolation_from (MidiSource* s)
{
	_interpolation_style = s->_interpolation_style;

	/* XXX: should probably emit signals here */
}

void
MidiSource::copy_automation_state_from (MidiSource* s)
{
	_automation_state = s->_automation_state;

	/* XXX: should probably emit signals here */
}
