/*
 *      editor.c - this file is part of Geany, a fast and lightweight IDE
 *
 *      Copyright 2005-2008 Enrico Tröger <enrico(dot)troeger(at)uvena(dot)de>
 *      Copyright 2006-2008 Nick Treleaven <nick(dot)treleaven(at)btinternet(dot)com>
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, write to the Free Software
 *      Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * $Id$
 */

/*
 * Callbacks for the Scintilla widget (ScintillaObject).
 * Most important is the sci-notify callback, handled in on_editor_notification().
 * This includes auto-indentation, comments, auto-completion, calltips, etc.
 * Also some general Scintilla-related functions.
 */

#include <ctype.h>
#include <string.h>

#include "SciLexer.h"
#include "geany.h"

#include "editor.h"
#include "document.h"
#include "filetypes.h"
#include "sciwrappers.h"
#include "ui_utils.h"
#include "utils.h"
#include "symbols.h"


/* holds word under the mouse or keyboard cursor */
static gchar current_word[GEANY_MAX_WORD_LENGTH];

/* Initialised in keyfile.c. */
EditorPrefs editor_prefs;

EditorInfo editor_info = {current_word, -1};

static struct
{
	gchar *text;
	gboolean set;
	gchar *last_word;
	guint tag_index;
} calltip = {NULL, FALSE, NULL, 0};

static gchar indent[100];


static void on_new_line_added(gint idx);
static gboolean handle_xml(gint idx, gchar ch);
static void get_indent(document *doc, gint pos, gboolean use_this_line);
static void auto_multiline(gint idx, gint pos);
static gboolean is_comment(gint lexer, gint style);
static void auto_close_bracket(ScintillaObject *sci, gint pos, gchar c);
static void editor_auto_table(document *doc, gint pos);


/* calls the edit popup menu in the editor */
gboolean
on_editor_button_press_event           (GtkWidget *widget,
                                        GdkEventButton *event,
                                        gpointer user_data)
{
	gint idx = GPOINTER_TO_INT(user_data);
	editor_info.click_pos = sci_get_position_from_xy(doc_list[idx].sci, (gint)event->x, (gint)event->y, FALSE);

	if (event->button == 1)
	{
		if (GDK_BUTTON_PRESS == event->type && editor_prefs.disable_dnd)
		{
			gint ss = sci_get_selection_start(doc_list[idx].sci);
			sci_set_selection_end(doc_list[idx].sci, ss);
		}
		return utils_check_disk_status(idx, FALSE);
	}

	if (event->button == 3)
	{
		editor_find_current_word(doc_list[idx].sci, editor_info.click_pos,
			current_word, sizeof current_word, NULL);

		ui_update_popup_goto_items((current_word[0] != '\0') ? TRUE : FALSE);
		ui_update_popup_copy_items(idx);
		ui_update_insert_include_item(idx, 0);
		gtk_menu_popup(GTK_MENU(app->popup_menu), NULL, NULL, NULL, NULL, event->button, event->time);

		return TRUE;
	}
	return FALSE;
}


typedef struct SCNotification SCNotification;

static void fold_symbol_click(ScintillaObject *sci, SCNotification *nt)
{
	gint line = SSM(sci, SCI_LINEFROMPOSITION, nt->position, 0);

	SSM(sci, SCI_TOGGLEFOLD, line, 0);
	/* extra toggling of child fold points
	 * use when editor_prefs.unfold_all_children is set and Shift is NOT pressed or when
	 * editor_prefs.unfold_all_children is NOT set but Shift is pressed */
	if ((editor_prefs.unfold_all_children && ! (nt->modifiers & SCMOD_SHIFT)) ||
		(! editor_prefs.unfold_all_children && (nt->modifiers & SCMOD_SHIFT)))
	{
		gint last_line = SSM(sci, SCI_GETLASTCHILD, line, -1);
		gint i;

		if (SSM(sci, SCI_GETLINEVISIBLE, line + 1, 0))
		{	/* unfold all children of the current fold point */
			for (i = line; i < last_line; i++)
			{
				if (! SSM(sci, SCI_GETLINEVISIBLE, i, 0))
				{
					SSM(sci, SCI_TOGGLEFOLD, SSM(sci, SCI_GETFOLDPARENT, i, 0), 0);
				}
			}
		}
		else
		{	/* fold all children of the current fold point */
			for (i = line; i < last_line; i++)
			{
				gint level = sci_get_fold_level(sci, i);
				if (level & SC_FOLDLEVELHEADERFLAG)
				{
					if (SSM(sci, SCI_GETFOLDEXPANDED, i, 0))
						SSM(sci, SCI_TOGGLEFOLD, i, 0);
				}
			}
		}
	}
}


static void on_margin_click(ScintillaObject *sci, SCNotification *nt)
{
	/* left click to marker margin marks the line */
	if (nt->margin == 1)
	{
		gint line = sci_get_line_from_position(sci, nt->position);
		gboolean set = sci_is_marker_set_at_line(sci, line, 1);

		/*sci_marker_delete_all(doc_list[idx].sci, 1);*/
		sci_set_marker_at_line(sci, line, ! set, 1);	/* toggle the marker */
	}
	/* left click on the folding margin to toggle folding state of current line */
	else if (nt->margin == 2 && editor_prefs.folding)
	{
		fold_symbol_click(sci, nt);
	}
}


static void on_update_ui(gint idx, G_GNUC_UNUSED SCNotification *nt)
{
	ScintillaObject *sci = doc_list[idx].sci;
	gint pos = sci_get_current_position(sci);

	/* undo / redo menu update */
	ui_update_popup_reundo_items(idx);

	/* brace highlighting */
	editor_highlight_braces(sci, pos);

	ui_update_statusbar(idx, pos);

	/* Visible lines are only laid out accurately once [SCN_UPDATEUI] is sent,
	 * so we need to only call sci_scroll_to_line here, because the document
	 * may have line wrapping and folding enabled.
	 * http://scintilla.sourceforge.net/ScintillaDoc.html#LineWrapping */
	if (doc_list[idx].scroll_percent > 0.0F)
	{
		editor_scroll_to_line(sci, -1, doc_list[idx].scroll_percent);
		doc_list[idx].scroll_percent = -1.0F;	/* disable further scrolling */
	}
#if 0
	/** experimental code for inverting selections */
	{
	gint i;
	for (i = SSM(sci, SCI_GETSELECTIONSTART, 0, 0); i < SSM(sci, SCI_GETSELECTIONEND, 0, 0); i++)
	{
		/* need to get colour from getstyleat(), but how? */
		SSM(sci, SCI_STYLESETFORE, STYLE_DEFAULT, 0);
		SSM(sci, SCI_STYLESETBACK, STYLE_DEFAULT, 0);
	}

	sci_get_style_at(sci, pos);
	}
#endif
}


static void on_char_added(gint idx, SCNotification *nt)
{
	ScintillaObject *sci = doc_list[idx].sci;
	gint pos = sci_get_current_position(sci);

	switch (nt->ch)
	{
		case '\r':
		{	/* simple indentation (only for CR format) */
			if (sci_get_eol_mode(sci) == SC_EOL_CR)
				on_new_line_added(idx);
			break;
		}
		case '\n':
		{	/* simple indentation (for CR/LF and LF format) */
			on_new_line_added(idx);
			break;
		}
		case '>':
		case '/':
		{	/* close xml-tags */
			handle_xml(idx, nt->ch);
			break;
		}
		case '(':
		{	/* show calltips */
			editor_show_calltip(idx, --pos);
			break;
		}
		case ')':
		{	/* hide calltips */
			if (SSM(sci, SCI_CALLTIPACTIVE, 0, 0))
			{
				SSM(sci, SCI_CALLTIPCANCEL, 0, 0);
			}
			g_free(calltip.text);
			calltip.text = NULL;
			calltip.set = FALSE;
			break;
		}
		case '[':
		case '{':
		{	/* Tex auto-closing */
			if (sci_get_lexer(sci) == SCLEX_LATEX)
			{
				auto_close_bracket(sci, pos, nt->ch);	/* Tex auto-closing */
				editor_show_calltip(idx, --pos);
			}
			break;
		}
		case '}':
		{	/* closing bracket handling */
			if (doc_list[idx].auto_indent)
				editor_close_block(idx, pos - 1);
			break;
		}
		default: editor_start_auto_complete(idx, pos, FALSE);
	}
}


/* expand() and fold_changed() are copied from SciTE (thanks) to fix #1923350. */
static void expand(ScintillaObject *sci, gint *line, gboolean doExpand,
		gboolean force, gint visLevels, gint level)
{
	gint lineMaxSubord = SSM(sci, SCI_GETLASTCHILD, *line, level & SC_FOLDLEVELNUMBERMASK);
	gint levelLine = level;
	(*line)++;
	while (*line <= lineMaxSubord)
	{
		if (force)
		{
			if (visLevels > 0)
				SSM(sci, SCI_SHOWLINES, *line, *line);
			else
				SSM(sci, SCI_HIDELINES, *line, *line);
		}
		else
		{
			if (doExpand)
				SSM(sci, SCI_SHOWLINES, *line, *line);
		}
		if (levelLine == -1)
			levelLine = SSM(sci, SCI_GETFOLDLEVEL, *line, 0);
		if (levelLine & SC_FOLDLEVELHEADERFLAG)
		{
			if (force)
			{
				if (visLevels > 1)
					SSM(sci, SCI_SETFOLDEXPANDED, *line, 1);
				else
					SSM(sci, SCI_SETFOLDEXPANDED, *line, 0);
				expand(sci, line, doExpand, force, visLevels - 1, -1);
			}
			else
			{
				if (doExpand)
				{
					if (!SSM(sci, SCI_GETFOLDEXPANDED, *line, 0))
						SSM(sci, SCI_SETFOLDEXPANDED, *line, 1);
					expand(sci, line, TRUE, force, visLevels - 1, -1);
				}
				else
				{
					expand(sci, line, FALSE, force, visLevels - 1, -1);
				}
			}
		}
		else
		{
			(*line)++;
		}
	}
}


static void fold_changed(ScintillaObject *sci, gint line, gint levelNow, gint levelPrev)
{
	if (levelNow & SC_FOLDLEVELHEADERFLAG)
	{
		if (! (levelPrev & SC_FOLDLEVELHEADERFLAG))
		{
			/* Adding a fold point */
			SSM(sci, SCI_SETFOLDEXPANDED, line, 1);
			expand(sci, &line, TRUE, FALSE, 0, levelPrev);
		}
	}
	else if (levelPrev & SC_FOLDLEVELHEADERFLAG)
	{
		if (! SSM(sci, SCI_GETFOLDEXPANDED, line, 0))
		{	/* Removing the fold from one that has been contracted so should expand
			 * otherwise lines are left invisible with no way to make them visible */
			SSM(sci, SCI_SETFOLDEXPANDED, line, 1);
			expand(sci, &line, TRUE, FALSE, 0, levelPrev);
		}
	}
	else if (! (levelNow & SC_FOLDLEVELWHITEFLAG) &&
			((levelPrev & SC_FOLDLEVELNUMBERMASK) > (levelNow & SC_FOLDLEVELNUMBERMASK)))
	{
		/* See if should still be hidden */
		gint parentLine = SSM(sci, SCI_GETFOLDPARENT, line, 0);
		if (parentLine < 0)
		{
			SSM(sci, SCI_SHOWLINES, line, line);
		}
		else if (SSM(sci, SCI_GETFOLDEXPANDED, parentLine, 0) &&
				SSM(sci, SCI_GETLINEVISIBLE, parentLine, 0))
		{
			SSM(sci, SCI_SHOWLINES, line, line);
		}
	}
}


static void ensure_range_visible(ScintillaObject *sci, gint posStart, gint posEnd,
		gboolean enforcePolicy)
{
	gint lineStart = SSM(sci, SCI_LINEFROMPOSITION, MIN(posStart, posEnd), 0);
	gint lineEnd = SSM(sci, SCI_LINEFROMPOSITION, MAX(posStart, posEnd), 0);
	gint line;

	for (line = lineStart; line <= lineEnd; line++)
	{
		SSM(sci, enforcePolicy ? SCI_ENSUREVISIBLEENFORCEPOLICY : SCI_ENSUREVISIBLE, line, 0);
	}
}


/* callback func called by all editors when a signal arises */
void on_editor_notification(GtkWidget *editor, gint scn, gpointer lscn, gpointer user_data)
{
	SCNotification *nt;
	ScintillaObject *sci;
	gint idx;

	idx = GPOINTER_TO_INT(user_data);
	sci = doc_list[idx].sci;

	nt = lscn;
	switch (nt->nmhdr.code)
	{
		case SCN_SAVEPOINTLEFT:
		{
			doc_list[idx].changed = TRUE;
			document_set_text_changed(idx);
			break;
		}
		case SCN_SAVEPOINTREACHED:
		{
			doc_list[idx].changed = FALSE;
			document_set_text_changed(idx);
			break;
		}
		case SCN_MODIFYATTEMPTRO:
		{
			utils_beep();
			break;
		}
		case SCN_MARGINCLICK:
			on_margin_click(sci, nt);
			break;

		case SCN_UPDATEUI:
			on_update_ui(idx, nt);
			break;

 		case SCN_MODIFIED:
		{
			if (nt->modificationType & SC_STARTACTION && ! app->ignore_callback)
			{
				/* get notified about undo changes */
				document_undo_add(idx, UNDO_SCINTILLA, NULL);
			}
			if (editor_prefs.folding && (nt->modificationType & SC_MOD_CHANGEFOLD) != 0)
			{
				/* handle special fold cases, e.g. #1923350 */
				fold_changed(sci, nt->line, nt->foldLevelNow, nt->foldLevelPrev);
			}
			break;
		}
		case SCN_CHARADDED:
			on_char_added(idx, nt);
			break;

		case SCN_USERLISTSELECTION:
		{
			if (nt->listType == 1)
			{
				gint pos = SSM(sci, SCI_GETCURRENTPOS, 0, 0);
				SSM(sci, SCI_INSERTTEXT, pos, (sptr_t) nt->text);
			}
			else if (nt->listType == 2)
			{
				gint start, pos = SSM(sci, SCI_GETCURRENTPOS, 0, 0);
				start = pos;
				while (start > 0 && sci_get_char_at(sci, --start) != '&') ;

				SSM(sci, SCI_INSERTTEXT, pos - 1, (sptr_t) nt->text);
			}
			break;
		}
		case SCN_AUTOCSELECTION:
		{
			/* now that autocomplete is finishing, reshow calltips if they were showing */
			if (calltip.set)
			{
				gint pos = sci_get_current_position(sci);
				SSM(sci, SCI_CALLTIPSHOW, pos, (sptr_t) calltip.text);
				/* now autocompletion has been cancelled, so do it manually */
				sci_set_selection_start(sci, nt->lParam);
				sci_set_selection_end(sci, pos);
				sci_replace_sel(sci, "");	/* clear root of word */
				SSM(sci, SCI_INSERTTEXT, nt->lParam, (sptr_t) nt->text);
				sci_goto_pos(sci, nt->lParam + strlen(nt->text), FALSE);
			}
			break;
		}
#ifdef GEANY_DEBUG
		case SCN_STYLENEEDED:
		{
			geany_debug("style");
			break;
		}
#endif
		case SCN_NEEDSHOWN:
		{
			ensure_range_visible(sci, nt->position, nt->position + nt->length, FALSE);
			break;
		}
		case SCN_URIDROPPED:
		{
			if (nt->text != NULL)
			{
				document_open_file_list(nt->text, -1);
			}
			break;
		}
		case SCN_CALLTIPCLICK:
		{
			if (nt->position > 0)
			{
				switch (nt->position)
				{
					case 1:	/* up arrow */
						if (calltip.tag_index > 0)
							calltip.tag_index--;
						break;

					case 2: calltip.tag_index++; break;	/* down arrow */
				}
				editor_show_calltip(idx, -1);
			}
			break;
		}
	}
}


/* Returns a string containing width chars of whitespace, filled with simple space
 * characters or with the right number of tab characters, according to the
 * use_tabs setting. (Result is filled with tabs *and* spaces if width isn't a multiple of
 * editor_prefs.tab_width). */
static gchar *
get_whitespace(gint width, gboolean use_tabs)
{
	gchar *str;

	g_return_val_if_fail(width > 0, NULL);

	if (use_tabs)
	{	/* first fill text with tabs and fill the rest with spaces */
		gint tabs = width / editor_prefs.tab_width;
		gint spaces = width % editor_prefs.tab_width;
		gint len = tabs + spaces;

		str = g_malloc(len + 1);

		memset(str, '\t', tabs);
		memset(str + tabs, ' ', spaces);
		str[len] = '\0';
 	}
	else
		str = g_strnfill(width, ' ');

	return str;
}


static void check_python_indent(gint idx, gint pos)
{
	document *doc = &doc_list[idx];
	gint last_char = pos - utils_get_eol_char_len(idx) - 1;

	/* add extra indentation for Python after colon */
	if (sci_get_char_at(doc->sci, last_char) == ':' &&
		sci_get_style_at(doc->sci, last_char) == SCE_P_OPERATOR)
	{
		/* creates and inserts one tabulator sign or
		 * whitespace of the amount of the tab width */
		gchar *text = get_whitespace(editor_prefs.tab_width, doc->use_tabs);
		sci_add_text(doc->sci, text);
		g_free(text);
	}
}


static void on_new_line_added(gint idx)
{
	ScintillaObject *sci = doc_list[idx].sci;
	gint pos = sci_get_current_position(sci);
	gint line = sci_get_current_line(sci);

	/* simple indentation */
	if (doc_list[idx].auto_indent)
	{
		get_indent(&doc_list[idx], pos, FALSE);
		sci_add_text(sci, indent);

		if (editor_prefs.indent_mode > INDENT_BASIC &&
			FILETYPE_ID(doc_list[idx].file_type) == GEANY_FILETYPES_PYTHON)
			check_python_indent(idx, pos);
	}

	if (editor_prefs.complete_snippets)
	{
		/* " * " auto completion in multiline C/C++/D/Java comments */
		auto_multiline(idx, pos);

		editor_auto_latex(idx, pos);
	}

	if (editor_prefs.newline_strip)
	{
		/* strip the trailing spaces on the previous line */
		document_strip_line_trailing_spaces(idx, line - 1);
	}
}


static gboolean lexer_has_braces(ScintillaObject *sci)
{
	gint lexer = SSM(sci, SCI_GETLEXER, 0, 0);

	switch (lexer)
	{
		case SCLEX_CPP:
		case SCLEX_D:
		case SCLEX_HTML:	/* for PHP & JS */
		case SCLEX_PASCAL:	/* for multiline comments? */
		case SCLEX_BASH:
		case SCLEX_PERL:
		case SCLEX_TCL:
			return TRUE;
		default:
			return FALSE;
	}
}


/* in place indentation of one tab or equivalent spaces */
static void do_indent(gchar *buf, gsize len, guint *idx, gboolean use_tabs)
{
	guint j = *idx;

	if (use_tabs)
	{
		if (j < len - 1)	/* leave room for a \0 terminator. */
			buf[j++] = '\t';
	}
	else
	{	/* insert as many spaces as a tab would take */
		guint k;
		for (k = 0; k < (guint) editor_prefs.tab_width && k < len - 1; k++)
			buf[j++] = ' ';
	}
	*idx = j;
}


/* "use_this_line" to auto-indent only if it is a real new line
 * and ignore the case of editor_close_block */
static void get_indent(document *doc, gint pos, gboolean use_this_line)
{
	ScintillaObject *sci = doc->sci;
	guint i, len, j = 0;
	gint prev_line;
	gchar *linebuf;

	prev_line = sci_get_line_from_position(sci, pos);

	if (! use_this_line)
		prev_line--;
	len = sci_get_line_length(sci, prev_line);
	linebuf = sci_get_line(sci, prev_line);

	for (i = 0; i < len && j <= (sizeof(indent) - 1); i++)
	{
		if (linebuf[i] == ' ' || linebuf[i] == '\t')	/* simple indentation */
			indent[j++] = linebuf[i];
		else if (editor_prefs.indent_mode <= INDENT_BASIC)
			break;
		else if (use_this_line)
			break;
		else	/* editor_close_block */
		{
			if (! lexer_has_braces(sci))
				break;

			/* i == (len - 1) prevents wrong indentation after lines like
			 * "	{ return bless({}, shift); }" (Perl) */
			if (linebuf[i] == '{' && i == (len - 1))
			{
				do_indent(indent, sizeof(indent), &j, doc->use_tabs);
				break;
			}
			else
			{
				gint k = len - 1;

				while (k > 0 && isspace(linebuf[k])) k--;

				/* if last non-whitespace character is a { increase indentation by a tab
				 * e.g. for (...) { */
				if (linebuf[k] == '{')
				{
					do_indent(indent, sizeof(indent), &j, doc->use_tabs);
				}
				break;
			}
		}
	}
	indent[j] = '\0';
	g_free(linebuf);
}


static void auto_close_bracket(ScintillaObject *sci, gint pos, gchar c)
{
	if (! editor_prefs.complete_snippets || SSM(sci, SCI_GETLEXER, 0, 0) != SCLEX_LATEX)
		return;

	if (c == '[')
	{
		sci_add_text(sci, "]");
	}
	else if (c == '{')
	{
		sci_add_text(sci, "}");
	}
	sci_set_current_position(sci, pos, TRUE);
}


/* Finds a corresponding matching brace to the given pos
 * (this is taken from Scintilla Editor.cxx,
 * fit to work with editor_close_block) */
static gint brace_match(ScintillaObject *sci, gint pos)
{
	gchar chBrace = sci_get_char_at(sci, pos);
	gchar chSeek = utils_brace_opposite(chBrace);
	gchar chAtPos;
	gint direction = -1;
	gint styBrace;
	gint depth = 1;
	gint styAtPos;

	styBrace = sci_get_style_at(sci, pos);

	if (utils_is_opening_brace(chBrace, editor_prefs.brace_match_ltgt))
		direction = 1;

	pos = pos + direction;
	while ((pos >= 0) && (pos < sci_get_length(sci)))
	{
		chAtPos = sci_get_char_at(sci, pos - 1);
		styAtPos = sci_get_style_at(sci, pos);

		if ((pos > sci_get_end_styled(sci)) || (styAtPos == styBrace))
		{
			if (chAtPos == chBrace)
				depth++;
			if (chAtPos == chSeek)
				depth--;
			if (depth == 0)
				return pos;
		}
		pos = pos + direction;
	}
	return -1;
}


/* Called after typing '}'. */
void editor_close_block(gint idx, gint pos)
{
	gint x = 0, cnt = 0;
	gint line, line_len, eol_char_len;
	gchar *text, *line_buf;
	ScintillaObject *sci;
	gint line_indent, last_indent;

	if (editor_prefs.indent_mode < INDENT_CURRENTCHARS)
		return;
	if (idx == -1 || ! doc_list[idx].is_valid || doc_list[idx].file_type == NULL)
		return;

	sci = doc_list[idx].sci;

	if (! lexer_has_braces(sci))
		return;

	line = sci_get_line_from_position(sci, pos);
	line_len = sci_get_line_length(sci, line);
	/* set eol_char_len to 0 if on last line, because there is no EOL char */
	eol_char_len = (line == (SSM(sci, SCI_GETLINECOUNT, 0, 0) - 1)) ? 0 :
								utils_get_eol_char_len(document_find_by_sci(sci));

	/* check that the line is empty, to not kill text in the line */
	line_buf = sci_get_line(sci, line);
	line_buf[line_len - eol_char_len] = '\0';
	while (x < (line_len - eol_char_len))
	{
		if (isspace(line_buf[x])) cnt++;
		x++;
	}
	g_free(line_buf);

	if ((line_len - eol_char_len - 1) != cnt)
		return;

	if (editor_prefs.indent_mode == INDENT_MATCHBRACES)
	{
		gint start_brace = brace_match(sci, pos);

		if (start_brace >= 0)
		{
			gint line_start;

			get_indent(&doc_list[idx], start_brace, TRUE);
			text = g_strconcat(indent, "}", NULL);
			line_start = sci_get_position_from_line(sci, line);
			sci_set_anchor(sci, line_start);
			SSM(sci, SCI_REPLACESEL, 0, (sptr_t) text);
			g_free(text);
			return;
		}
		/* fall through - unmatched brace (possibly because of TCL, PHP lexer bugs) */
	}

	/* INDENT_CURRENTCHARS */
	line_indent = sci_get_line_indentation(sci, line);
	last_indent = sci_get_line_indentation(sci, line - 1);

	if (line_indent < last_indent)
		return;
	line_indent -= editor_prefs.tab_width;
	line_indent = MAX(0, line_indent);
	sci_set_line_indentation(sci, line, line_indent);
}


/* Reads the word at given cursor position and writes it into the given buffer. The buffer will be
 * NULL terminated in any case, even when the word is truncated because wordlen is too small.
 * position can be -1, then the current position is used.
 * wc are the wordchars to use, if NULL, GEANY_WORDCHARS will be used */
void editor_find_current_word(ScintillaObject *sci, gint pos, gchar *word, size_t wordlen,
							  const gchar *wc)
{
	gint line, line_start, startword, endword;
	gchar *chunk;

	if (pos == -1)
		pos = sci_get_current_position(sci);

	line = sci_get_line_from_position(sci, pos);
	line_start = sci_get_position_from_line(sci, line);
	startword = pos - line_start;
	endword = pos - line_start;

	word[0] = '\0';
	chunk = sci_get_line(sci, line);

	if (wc == NULL)
		wc = GEANY_WORDCHARS;

	while (startword > 0 && strchr(wc, chunk[startword - 1]))
		startword--;
	while (chunk[endword] && strchr(wc, chunk[endword]))
		endword++;
	if(startword == endword)
		return;

	chunk[endword] = '\0';

	g_strlcpy(word, chunk + startword, wordlen); /* ensure null terminated */
	g_free(chunk);
}


static gint find_previous_brace(ScintillaObject *sci, gint pos)
{
	gchar c;
	gint orig_pos = pos;

	c = SSM(sci, SCI_GETCHARAT, pos, 0);
	while (pos >= 0 && pos > orig_pos - 300)
	{
		c = SSM(sci, SCI_GETCHARAT, pos, 0);
		pos--;
		if (utils_is_opening_brace(c, editor_prefs.brace_match_ltgt))
			return pos;
	}
	return -1;
}


static gint find_start_bracket(ScintillaObject *sci, gint pos)
{
	gchar c;
	gint brackets = 0;
	gint orig_pos = pos;

	c = SSM(sci, SCI_GETCHARAT, pos, 0);
	while (pos > 0 && pos > orig_pos - 300)
	{
		c = SSM(sci, SCI_GETCHARAT, pos, 0);
		if (c == ')') brackets++;
		else if (c == '(') brackets--;
		pos--;
		if (brackets < 0) return pos;	/* found start bracket */
	}
	return -1;
}


static gboolean append_calltip(GString *str, const TMTag *tag, filetype_id ft_id)
{
	if (! tag->atts.entry.arglist) return FALSE;

	if (tag->atts.entry.var_type)
	{
		guint i;

		g_string_append(str, tag->atts.entry.var_type);
		for (i = 0; i < tag->atts.entry.pointerOrder; i++)
		{
			g_string_append_c(str, '*');
		}
		g_string_append_c(str, ' ');
	}
	if (tag->atts.entry.scope)
	{
		const gchar *cosep = symbols_get_context_separator(ft_id);

		g_string_append(str, tag->atts.entry.scope);
		g_string_append(str, cosep);
	}
	g_string_append(str, tag->name);
	g_string_append_c(str, ' ');
	g_string_append(str, tag->atts.entry.arglist);

	return TRUE;
}


static gchar *find_calltip(const gchar *word, filetype *ft)
{
	const GPtrArray *tags;
	const gint arg_types = tm_tag_function_t | tm_tag_prototype_t |
		tm_tag_method_t | tm_tag_macro_with_arg_t;
	TMTagAttrType *attrs = NULL;
	TMTag *tag;
	GString *str = NULL;
	guint i;

	g_return_val_if_fail(ft && word && *word, NULL);

	tags = tm_workspace_find(word, arg_types | tm_tag_class_t, attrs, FALSE, ft->lang);
	if (tags->len == 0)
		return NULL;

	tag = TM_TAG(tags->pdata[0]);

	if (tag->type == tm_tag_class_t && FILETYPE_ID(ft) == GEANY_FILETYPES_D)
	{
		/* user typed e.g. 'new Classname(' so lookup D constructor Classname::this() */
		tags = tm_workspace_find_scoped("this", tag->name,
			arg_types, attrs, FALSE, ft->lang, TRUE);
		if (tags->len == 0)
			return NULL;
	}

	/* remove tags with no argument list */
	for (i = 0; i < tags->len; i++)
	{
		tag = TM_TAG(tags->pdata[i]);

		if (! tag->atts.entry.arglist)
			tags->pdata[i] = NULL;
	}
	tm_tags_prune((GPtrArray *) tags);
	if (tags->len == 0)
		return NULL;
	else
	{	/* remove duplicate calltips */
		TMTagAttrType sort_attr[] = {tm_tag_attr_name_t, tm_tag_attr_scope_t,
			tm_tag_attr_arglist_t, 0};

		tm_tags_sort((GPtrArray *) tags, sort_attr, TRUE);
	}

	/* if the current word has changed since last time, start with the first tag match */
	if (! utils_str_equal(word, calltip.last_word))
		calltip.tag_index = 0;
	/* cache the current word for next time */
	g_free(calltip.last_word);
	calltip.last_word = g_strdup(word);
	calltip.tag_index = MIN(calltip.tag_index, tags->len - 1);	/* ensure tag_index is in range */

	for (i = calltip.tag_index; i < tags->len; i++)
	{
		tag = TM_TAG(tags->pdata[i]);

		if (str == NULL)
		{
			str = g_string_new(NULL);
			if (calltip.tag_index > 0)
				g_string_prepend(str, "\001 ");	/* up arrow */
			append_calltip(str, tag, FILETYPE_ID(ft));
		}
		else /* add a down arrow */
		{
			if (calltip.tag_index > 0)	/* already have an up arrow */
				g_string_insert_c(str, 1, '\002');
			else
				g_string_prepend(str, "\002 ");
			break;
		}
	}
	if (str)
	{
		gchar *result = str->str;

		g_string_free(str, FALSE);
		return result;
	}
	return NULL;
}


/* use pos = -1 to search for the previous unmatched open bracket. */
gboolean editor_show_calltip(gint idx, gint pos)
{
	gint orig_pos = pos; /* the position for the calltip */
	gint lexer;
	gint style;
	gchar word[GEANY_MAX_WORD_LENGTH];
	gchar *str;
	ScintillaObject *sci;

	if (idx == -1 || ! doc_list[idx].is_valid || doc_list[idx].file_type == NULL) return FALSE;
	sci = doc_list[idx].sci;

	lexer = SSM(sci, SCI_GETLEXER, 0, 0);

	if (pos == -1)
	{
		/* position of '(' is unknown, so go backwards from current position to find it */
		pos = SSM(sci, SCI_GETCURRENTPOS, 0, 0);
		pos--;
		orig_pos = pos;
		pos = (lexer == SCLEX_LATEX) ? find_previous_brace(sci, pos) :
			find_start_bracket(sci, pos);
		if (pos == -1) return FALSE;
	}

	/* the style 1 before the brace (which may be highlighted) */
	style = SSM(sci, SCI_GETSTYLEAT, pos - 1, 0);
	if (is_comment(lexer, style))
		return FALSE;

	word[0] = '\0';
	editor_find_current_word(sci, pos - 1, word, sizeof word, NULL);
	if (word[0] == '\0') return FALSE;

	str = find_calltip(word, doc_list[idx].file_type);
	if (str)
	{
		g_free(calltip.text);	/* free the old calltip */
		calltip.text = str;
		calltip.set = TRUE;
		utils_wrap_string(calltip.text, -1);
		SSM(sci, SCI_CALLTIPSHOW, orig_pos, (sptr_t) calltip.text);
		return TRUE;
	}
	return FALSE;
}


static void show_autocomplete(ScintillaObject *sci, gint rootlen, const gchar *words)
{
	/* store whether a calltip is showing, so we can reshow it after autocompletion */
	calltip.set = SSM(sci, SCI_CALLTIPACTIVE, 0, 0);
	SSM(sci, SCI_AUTOCSHOW, rootlen, (sptr_t) words);
}


static gboolean
autocomplete_html(ScintillaObject *sci, const gchar *root, gsize rootlen)
{	/* HTML entities auto completion */
	guint i, j = 0;
	GString *words;
	const gchar **entities = symbols_get_html_entities();

	if (*root != '&' || entities == NULL) return FALSE;

	words = g_string_sized_new(500);
	for (i = 0; ; i++)
	{
		if (entities[i] == NULL) break;
		else if (entities[i][0] == '#') continue;

		if (! strncmp(entities[i], root, rootlen))
		{
			if (j++ > 0) g_string_append_c(words, '\n');
			g_string_append(words, entities[i]);
		}
	}
	if (words->len > 0) show_autocomplete(sci, rootlen, words->str);
	g_string_free(words, TRUE);
	return TRUE;
}


static gboolean
autocomplete_tags(gint idx, gchar *root, gsize rootlen)
{	/* PHP, LaTeX, C, C++, D and Java tag autocompletion */
	TMTagAttrType attrs[] = { tm_tag_attr_name_t, 0 };
	const GPtrArray *tags;
	ScintillaObject *sci;

	if (! DOC_IDX_VALID(idx) || doc_list[idx].file_type == NULL)
		return FALSE;
	sci = doc_list[idx].sci;

	tags = tm_workspace_find(root, tm_tag_max_t, attrs, TRUE, doc_list[idx].file_type->lang);
	if (NULL != tags && tags->len > 0)
	{
		GString *words = g_string_sized_new(150);
		guint j;

		for (j = 0; ((j < tags->len) && (j < GEANY_MAX_AUTOCOMPLETE_WORDS)); ++j)
		{
			if (j > 0) g_string_append_c(words, '\n');
			g_string_append(words, ((TMTag *) tags->pdata[j])->name);
		}
		show_autocomplete(sci, rootlen, words->str);
		g_string_free(words, TRUE);
	}
	return TRUE;
}


gboolean editor_start_auto_complete(gint idx, gint pos, gboolean force)
{
	gint line, line_start, line_len, line_pos, current, rootlen, startword, lexer, style;
	gchar *linebuf, *root;
	ScintillaObject *sci;
	gboolean ret = FALSE;
	gchar *wordchars;
	filetype *ft;

	if ((! editor_prefs.auto_complete_symbols && ! force) ||
		! DOC_IDX_VALID(idx) || doc_list[idx].file_type == NULL)
		return FALSE;

	sci = doc_list[idx].sci;
	ft = doc_list[idx].file_type;

	line = sci_get_line_from_position(sci, pos);
	line_start = sci_get_position_from_line(sci, line);
	line_len = sci_get_line_length(sci, line);
	line_pos = pos - line_start - 1;
	current = pos - line_start;
	startword = current;
	lexer = SSM(sci, SCI_GETLEXER, 0, 0);
	style = SSM(sci, SCI_GETSTYLEAT, pos, 0);

	 /* don't autocomplete in comments and strings */
	 if (!force && is_comment(lexer, style))
		return FALSE;

	linebuf = sci_get_line(sci, line);

	if (ft->id == GEANY_FILETYPES_LATEX)
		wordchars = GEANY_WORDCHARS"\\"; /* add \ to word chars if we are in a LaTeX file */
	else if (ft->id == GEANY_FILETYPES_HTML || ft->id == GEANY_FILETYPES_PHP)
		wordchars = GEANY_WORDCHARS"&"; /* add & to word chars if we are in a PHP or HTML file */
	else
		wordchars = GEANY_WORDCHARS;

	/* find the start of the current word */
	while ((startword > 0) && (strchr(wordchars, linebuf[startword - 1])))
		startword--;
	linebuf[current] = '\0';
	root = linebuf + startword;
	rootlen = current - startword;

	/* entity autocompletion always in a HTML file, in a PHP file only
	 * when we are outside of <? ?> */
	if (ft->id == GEANY_FILETYPES_HTML ||
		(ft->id == GEANY_FILETYPES_PHP && (style < SCE_HPHP_DEFAULT || style > SCE_HPHP_OPERATOR) &&
		 line != (sci_get_line_count(sci) - 1))) /* this check is a workaround for a Scintilla bug:
												  * the last line in a PHP gets wrong styling */
		ret = autocomplete_html(sci, root, rootlen);
	else
	{
		/* force is set when called by keyboard shortcut, otherwise start at the
		 * editor_prefs.symbolcompletion_min_chars'th char */
		if (force || rootlen >= editor_prefs.symbolcompletion_min_chars)
			ret = autocomplete_tags(idx, root, rootlen);
	}

	g_free(linebuf);
	return ret;
}


void editor_auto_latex(gint idx, gint pos)
{
	ScintillaObject *sci;

	if (idx == -1 || ! doc_list[idx].is_valid || doc_list[idx].file_type == NULL) return;
	sci = doc_list[idx].sci;

	if (sci_get_char_at(sci, pos - 2) == '}')
	{
		gchar *eol, *buf, *construct;
		gchar env[50];
		gint line = sci_get_line_from_position(sci, pos - 2);
		gint line_len = sci_get_line_length(sci, line);
		gint i, start;

		/* get the line */
		buf = sci_get_line(sci, line);

		/* get to the first non-blank char (some kind of ltrim()) */
		start = 0;
		/*while (isspace(buf[i++])) start++;*/
		while (isspace(buf[start])) start++;

		/* check for begin */
		if (strncmp(buf + start, "\\begin", 6) == 0)
		{
			gchar full_cmd[15];
			guint j = 0;

			/* take also "\begingroup" (or whatever there can be) and
			 * append "\endgroup" and so on. */
			i = start + 6;
			while (i < line_len && buf[i] != '{' && j < (sizeof(full_cmd) - 1))
			{	/* copy all between "\begin" and "{" to full_cmd */
				full_cmd[j] = buf[i];
				i++;
				j++;
			}
			full_cmd[j] = '\0';

			/* go through the line and get the environment */
			for (i = start + j; i < line_len; i++)
			{
				if (buf[i] == '{')
				{
					j = 0;
					i++;
					while (buf[i] != '}' && j < (sizeof(env) - 1))
					{	/* this could be done in a shorter way, but so it remains readable ;-) */
						env[j] = buf[i];
						j++;
						i++;
					}
					env[j] = '\0';
					break;
				}
			}

			/* get the indentation */
			if (doc_list[idx].auto_indent) get_indent(&doc_list[idx], pos, TRUE);
			eol = g_strconcat(utils_get_eol_char(idx), indent, NULL);

			construct = g_strdup_printf("%s\\end%s{%s}", eol, full_cmd, env);

			SSM(sci, SCI_INSERTTEXT, pos, (sptr_t) construct);
			sci_goto_pos(sci, pos + 1, TRUE);
			g_free(construct);
			g_free(eol);
		}
		/* later there could be some else ifs for other keywords */

		g_free(buf);
	}
}


static gchar *snippets_find_completion_by_name(const gchar *type, const gchar *name)
{
	gchar *result = NULL;
	GHashTable *tmp;

	g_return_val_if_fail(type != NULL && name != NULL, NULL);

	tmp = g_hash_table_lookup(editor_prefs.snippets, type);
	if (tmp != NULL)
	{
		result = g_hash_table_lookup(tmp, name);
	}
	/* whether nothing is set for the current filetype(tmp is NULL) or
	 * the particular completion for this filetype is not set (result is NULL) */
	if (tmp == NULL || result == NULL)
	{
		tmp = g_hash_table_lookup(editor_prefs.snippets, "Default");
		if (tmp != NULL)
		{
			result = g_hash_table_lookup(tmp, name);
		}
	}
	/* if result is still NULL here, no completion could be found */

	/* result is owned by the hash table and will be freed when the table will destroyed */
	return g_strdup(result);
}


/* This is very ugly but passing the pattern to ac_replace_specials() doesn't work because it is
 * modified when replacing a completion but the foreach function still passes the old pointer
 * to ac_replace_specials, so we use a global pointer outside of ac_replace_specials and
 * ac_complete_constructs. Any hints to improve this are welcome. */
static gchar *snippets_global_pattern = NULL;

void snippets_replace_specials(gpointer key, gpointer value, gpointer user_data)
{
	gchar *needle;

	if (key == NULL || value == NULL)
		return;

	needle = g_strconcat("%", (gchar*) key, "%", NULL);

	snippets_global_pattern = utils_str_replace(snippets_global_pattern, needle, (gchar*) value);
	g_free(needle);
}


static gboolean snippets_complete_constructs(gint idx, gint pos, const gchar *word)
{
	gchar *str;
	gchar *pattern;
	gchar *lindent;
	gchar *whitespace;
	gint step, str_len;
	gint ft_id = FILETYPE_ID(doc_list[idx].file_type);
	GHashTable *specials;
	ScintillaObject *sci = doc_list[idx].sci;

	str = g_strdup(word);
	g_strstrip(str);

	pattern = snippets_find_completion_by_name(filetypes[ft_id]->name, str);
	if (pattern == NULL || pattern[0] == '\0')
	{
		utils_free_pointers(str, pattern, NULL); /* free pattern in case it is "" */
		return FALSE;
	}

	get_indent(&doc_list[idx], pos, TRUE);
	lindent = g_strconcat(utils_get_eol_char(idx), indent, NULL);
	whitespace = get_whitespace(editor_prefs.tab_width, doc_list[idx].use_tabs);

	/* remove the typed word, it will be added again by the used auto completion
	 * (not really necessary but this makes the auto completion more flexible,
	 *  e.g. with a completion like hi=hello, so typing "hi<TAB>" will result in "hello") */
	str_len = strlen(str);
	sci_set_selection_start(sci, pos - str_len);
	sci_set_selection_end(sci, pos);
	sci_replace_sel(sci, "");
	pos -= str_len; /* pos has changed while deleting */

	/* replace 'special' completions */
	specials = g_hash_table_lookup(editor_prefs.snippets, "Special");
	if (specials != NULL)
	{
		/* ugly hack using global_pattern */
		snippets_global_pattern = pattern;
		g_hash_table_foreach(specials, snippets_replace_specials, NULL);
		pattern = snippets_global_pattern;
	}

	/* replace line breaks and whitespaces */
	pattern = utils_str_replace(pattern, "\n", "%newline%"); /* to avoid endless replacing of \n */
	pattern = utils_str_replace(pattern, "%newline%", lindent);

	pattern = utils_str_replace(pattern, "\t", "%ws%"); /* to avoid endless replacing of \t */
	pattern = utils_str_replace(pattern, "%ws%", whitespace);

	/* find the %cursor% pos (has to be done after all other operations) */
	step = utils_strpos(pattern, "%cursor%");
	if (step != -1)
		pattern = utils_str_replace(pattern, "%cursor%", "");

	/* finally insert the text and set the cursor */
	SSM(sci, SCI_INSERTTEXT, pos, (sptr_t) pattern);
	if (step != -1)
		sci_goto_pos(sci, pos + step, TRUE);
	else
		sci_goto_pos(sci, pos + strlen(pattern), TRUE);

	utils_free_pointers(pattern, whitespace, lindent, str, NULL);
 	return TRUE;
}


static gboolean at_eol(ScintillaObject *sci, gint pos)
{
	gint line = sci_get_line_from_position(sci, pos);
	gchar c;

	/* skip any trailing spaces */
	while (TRUE)
	{
		c = sci_get_char_at(sci, pos);
		if (c == ' ' || c == '\t')
			pos++;
		else
			break;
	}

	return (pos == sci_get_line_end_position(sci, line));
}


gboolean editor_complete_snippet(gint idx, gint pos)
{
	gboolean result = FALSE;
	gint lexer, style;
	gchar *wc;
	ScintillaObject *sci;

	if (! DOC_IDX_VALID(idx))
		return FALSE;

	sci = doc_list[idx].sci;
	/* return if we are editing an existing line (chars on right of cursor) */
	if (! editor_prefs.complete_snippets_whilst_editing && ! at_eol(sci, pos))
		return FALSE;

	lexer = SSM(sci, SCI_GETLEXER, 0, 0);
	style = SSM(sci, SCI_GETSTYLEAT, pos - 2, 0);

	wc = snippets_find_completion_by_name("Special", "wordchars");
	editor_find_current_word(sci, pos, current_word, sizeof current_word, wc);

	/* prevent completion of "for " */
	if (! isspace(sci_get_char_at(sci, pos - 1))) /* pos points to the line end char so use pos -1 */
	{
		sci_start_undo_action(sci);	/* needed because we insert a space separately from construct */
		result = snippets_complete_constructs(idx, pos, current_word);
		sci_end_undo_action(sci);
	}

	g_free(wc);
	return result;
}


void editor_show_macro_list(ScintillaObject *sci)
{
	GString *words;

	if (sci == NULL) return;

	words = symbols_get_macro_list();
	if (words == NULL) return;

	SSM(sci, SCI_USERLISTSHOW, 1, (sptr_t) words->str);
	g_string_free(words, TRUE);
}


/**
 * (stolen from anjuta and heavily modified)
 * This routine will auto complete XML or HTML tags that are still open by closing them
 * @param ch The character we are dealing with, currently only works with the '>' character
 * @return True if handled, false otherwise
 */
static gboolean handle_xml(gint idx, gchar ch)
{
	ScintillaObject *sci = doc_list[idx].sci;
	gint lexer = SSM(sci, SCI_GETLEXER, 0, 0);
	gint pos, min;
	gchar *str_found, sel[512];

	/* If the user has turned us off, quit now.
	 * This may make sense only in certain languages */
	if (! editor_prefs.auto_close_xml_tags || (lexer != SCLEX_HTML && lexer != SCLEX_XML))
		return FALSE;

	pos = sci_get_current_position(sci);

	/* return if we are in PHP but not in a string or outside of <? ?> tags */
	if (doc_list[idx].file_type->id == GEANY_FILETYPES_PHP)
	{
		gint style = sci_get_style_at(sci, pos);
		if (style != SCE_HPHP_SIMPLESTRING && style != SCE_HPHP_HSTRING &&
			style <= SCE_HPHP_OPERATOR && style >= SCE_HPHP_DEFAULT)
			return FALSE;
	}

	/* if ch is /, check for </, else quit */
	if (ch == '/' && sci_get_char_at(sci, pos - 2) != '<')
		return FALSE;

	/* Grab the last 512 characters or so */
	min = pos - (sizeof(sel) - 1);
	if (min < 0) min = 0;

	if (pos - min < 3)
		return FALSE; /* Smallest tag is 3 characters e.g. <p> */

	sci_get_text_range(sci, min, pos, sel);
	sel[sizeof(sel) - 1] = '\0';

	if (ch == '>' && sel[pos - min - 2] == '/')
		/* User typed something like "<br/>" */
		return FALSE;

	str_found = utils_find_open_xml_tag(sel, pos - min, (ch == '/'));

	/* when found string is something like br, img or another short tag, quit */
	if (utils_str_equal(str_found, "br")
	 || utils_str_equal(str_found, "img")
	 || utils_str_equal(str_found, "base")
	 || utils_str_equal(str_found, "basefont")	/* < or not < */
	 || utils_str_equal(str_found, "frame")
	 || utils_str_equal(str_found, "input")
	 || utils_str_equal(str_found, "link")
	 || utils_str_equal(str_found, "area")
	 || utils_str_equal(str_found, "meta"))
	{
		return FALSE;
	}

	if (*str_found != '\0')
	{
		gchar *to_insert;
		if (ch == '/')
		{
			gchar *gt = ">";
			/* if there is already a '>' behind the cursor, don't add it */
			if (sci_get_char_at(sci, pos) == '>')
				gt = "";

			to_insert = g_strconcat(str_found, gt, NULL);
		}
		else
			to_insert = g_strconcat("</", str_found, ">", NULL);
		sci_start_undo_action(sci);
		sci_replace_sel(sci, to_insert);
		if (ch == '>')
		{
			SSM(sci, SCI_SETSEL, pos, pos);
			if (utils_str_equal(str_found, "table"))
				editor_auto_table(&doc_list[idx], pos);
		}
		sci_end_undo_action(sci);
		g_free(to_insert);
		g_free(str_found);
		return TRUE;
	}

	g_free(str_found);
	return FALSE;
}


static void editor_auto_table(document *doc, gint pos)
{
	ScintillaObject *sci = doc->sci;
	gchar *table;
	gint indent_pos;

	if (SSM(sci, SCI_GETLEXER, 0, 0) != SCLEX_HTML) return;

	get_indent(doc, pos, TRUE);
	indent_pos = sci_get_line_indent_position(sci, sci_get_line_from_position(sci, pos));
	if ((pos - 7) != indent_pos) /* 7 == strlen("<table>") */
	{
		gint i, x;
		x = strlen(indent);
		/* find the start of the <table tag */
		i = 1;
		while (i <= pos && sci_get_char_at(sci, pos - i) != '<') i++;
		/* add all non whitespace before the tag to the indent string */
		while ((pos - i) != indent_pos)
		{
			indent[x++] = ' ';
			i++;
		}
		indent[x] = '\0';
	}

	table = g_strconcat("\n", indent, "    <tr>\n", indent, "        <td>\n", indent, "        </td>\n",
						indent, "    </tr>\n", indent, NULL);
	sci_insert_text(sci, pos, table);
	g_free(table);
}


static void real_comment_multiline(gint idx, gint line_start, gint last_line)
{
	const gchar *eol;
	gchar *str_begin, *str_end;
	gint line_len;

	if (idx == -1 || ! doc_list[idx].is_valid || doc_list[idx].file_type == NULL) return;

	eol = utils_get_eol_char(idx);
	str_begin = g_strdup_printf("%s%s", doc_list[idx].file_type->comment_open, eol);
	str_end = g_strdup_printf("%s%s", doc_list[idx].file_type->comment_close, eol);

	/* insert the comment strings */
	sci_insert_text(doc_list[idx].sci, line_start, str_begin);
	line_len = sci_get_position_from_line(doc_list[idx].sci, last_line + 2);
	sci_insert_text(doc_list[idx].sci, line_len, str_end);

	g_free(str_begin);
	g_free(str_end);
}


static void real_uncomment_multiline(gint idx)
{
	/* find the beginning of the multi line comment */
	gint pos, line, len, x;
	gchar *linebuf;

	if (idx == -1 || ! doc_list[idx].is_valid || doc_list[idx].file_type == NULL) return;

	/* remove comment open chars */
	pos = document_find_text(idx, doc_list[idx].file_type->comment_open, 0, TRUE, FALSE, NULL);
	SSM(doc_list[idx].sci, SCI_DELETEBACK, 0, 0);

	/* check whether the line is empty and can be deleted */
	line = sci_get_line_from_position(doc_list[idx].sci, pos);
	len = sci_get_line_length(doc_list[idx].sci, line);
	linebuf = sci_get_line(doc_list[idx].sci, line);
	x = 0;
	while (linebuf[x] != '\0' && isspace(linebuf[x])) x++;
	if (x == len) SSM(doc_list[idx].sci, SCI_LINEDELETE, 0, 0);
	g_free(linebuf);

	/* remove comment close chars */
	pos = document_find_text(idx, doc_list[idx].file_type->comment_close, 0, FALSE, FALSE, NULL);
	SSM(doc_list[idx].sci, SCI_DELETEBACK, 0, 0);

	/* check whether the line is empty and can be deleted */
	line = sci_get_line_from_position(doc_list[idx].sci, pos);
	len = sci_get_line_length(doc_list[idx].sci, line);
	linebuf = sci_get_line(doc_list[idx].sci, line);
	x = 0;
	while (linebuf[x] != '\0' && isspace(linebuf[x])) x++;
	if (x == len) SSM(doc_list[idx].sci, SCI_LINEDELETE, 0, 0);
	g_free(linebuf);
}


/* set toggle to TRUE if the caller is the toggle function, FALSE otherwise
 * returns the amount of uncommented single comment lines, in case of multi line uncomment
 * it returns just 1 */
gint editor_do_uncomment(gint idx, gint line, gboolean toggle)
{
	gint first_line, last_line;
	gint x, i, line_start, line_len;
	gint sel_start, sel_end;
	gint count = 0;
	gsize co_len;
	gchar sel[256], *co, *cc;
	gboolean break_loop = FALSE, single_line = FALSE;
	filetype *ft;

	if (! DOC_IDX_VALID(idx) || doc_list[idx].file_type == NULL)
		return 0;

	if (line < 0)
	{	/* use selection or current line */
		sel_start = sci_get_selection_start(doc_list[idx].sci);
		sel_end = sci_get_selection_end(doc_list[idx].sci);

		first_line = sci_get_line_from_position(doc_list[idx].sci, sel_start);
		/* Find the last line with chars selected (not EOL char) */
		last_line = sci_get_line_from_position(doc_list[idx].sci,
			sel_end - utils_get_eol_char_len(idx));
		last_line = MAX(first_line, last_line);
	}
	else
	{
		first_line = last_line = line;
		sel_start = sel_end = sci_get_position_from_line(doc_list[idx].sci, line);
	}

	ft = doc_list[idx].file_type;

	/* detection of HTML vs PHP code, if non-PHP set filetype to XML */
	line_start = sci_get_position_from_line(doc_list[idx].sci, first_line);
	if (ft->id == GEANY_FILETYPES_PHP)
	{
		if (sci_get_style_at(doc_list[idx].sci, line_start) < 118 ||
			sci_get_style_at(doc_list[idx].sci, line_start) > 127)
			ft = filetypes[GEANY_FILETYPES_XML];
	}

	co = ft->comment_open;
	cc = ft->comment_close;
	if (co == NULL)
		return 0;

	co_len = strlen(co);
	if (co_len == 0)
		return 0;

	SSM(doc_list[idx].sci, SCI_BEGINUNDOACTION, 0, 0);

	for (i = first_line; (i <= last_line) && (! break_loop); i++)
	{
		gint buf_len;

		line_start = sci_get_position_from_line(doc_list[idx].sci, i);
		line_len = sci_get_line_length(doc_list[idx].sci, i);
		x = 0;

		buf_len = MIN((gint)sizeof(sel) - 1, line_len - 1);
		if (buf_len <= 0)
			continue;
		sci_get_text_range(doc_list[idx].sci, line_start, line_start + buf_len, sel);
		sel[buf_len] = '\0';

		while (isspace(sel[x])) x++;

		/* to skip blank lines */
		if (x < line_len && sel[x] != '\0')
		{
			/* use single line comment */
			if (cc == NULL || strlen(cc) == 0)
			{
				gsize tm_len = strlen(GEANY_TOGGLE_MARK);

				single_line = TRUE;

				if (toggle)
				{
					if (strncmp(sel + x, co, co_len) != 0 ||
						strncmp(sel + x + co_len, GEANY_TOGGLE_MARK, tm_len) != 0)
						continue;

					co_len += tm_len;
				}
				else
				{
					if (strncmp(sel + x, co, co_len) != 0)
						continue;
				}

				SSM(doc_list[idx].sci, SCI_SETSEL, line_start + x, line_start + x + co_len);
				sci_replace_sel(doc_list[idx].sci, "");
				count++;
			}
			/* use multi line comment */
			else
			{
				gint style_comment;
				gint lexer = SSM(doc_list[idx].sci, SCI_GETLEXER, 0, 0);

				/* process only lines which are already comments */
				switch (lexer)
				{	/* I will list only those lexers which support multi line comments */
					case SCLEX_XML:
					case SCLEX_HTML:
					{
						if (sci_get_style_at(doc_list[idx].sci, line_start) >= 118 &&
							sci_get_style_at(doc_list[idx].sci, line_start) <= 127)
							style_comment = SCE_HPHP_COMMENT;
						else style_comment = SCE_H_COMMENT;
						break;
					}
					case SCLEX_CSS: style_comment = SCE_CSS_COMMENT; break;
					case SCLEX_SQL: style_comment = SCE_SQL_COMMENT; break;
					case SCLEX_CAML: style_comment = SCE_CAML_COMMENT; break;
					case SCLEX_D: style_comment = SCE_D_COMMENT; break;
					default: style_comment = SCE_C_COMMENT;
				}
				if (sci_get_style_at(doc_list[idx].sci, line_start + x) == style_comment)
				{
					real_uncomment_multiline(idx);
					count = 1;
				}

				/* break because we are already on the last line */
				break_loop = TRUE;
				break;
			}
		}
	}
	SSM(doc_list[idx].sci, SCI_ENDUNDOACTION, 0, 0);

	/* restore selection if there is one
	 * but don't touch the selection if caller is editor_do_comment_toggle */
	if (! toggle && sel_start < sel_end)
	{
		if (single_line)
		{
			sci_set_selection_start(doc_list[idx].sci, sel_start - co_len);
			sci_set_selection_end(doc_list[idx].sci, sel_end - (count * co_len));
		}
		else
		{
			gint eol_len = utils_get_eol_char_len(idx);
			sci_set_selection_start(doc_list[idx].sci, sel_start - co_len - eol_len);
			sci_set_selection_end(doc_list[idx].sci, sel_end - co_len - eol_len);
		}
	}

	return count;
}


void editor_do_comment_toggle(gint idx)
{
	gint first_line, last_line;
	gint x, i, line_start, line_len, first_line_start;
	gint sel_start, sel_end;
	gint count_commented = 0, count_uncommented = 0;
	gchar sel[256], *co, *cc;
	gboolean break_loop = FALSE, single_line = FALSE;
	gboolean first_line_was_comment = FALSE;
	gsize co_len;
	gsize tm_len = strlen(GEANY_TOGGLE_MARK);
	filetype *ft;

	if (! DOC_IDX_VALID(idx) || doc_list[idx].file_type == NULL)
		return;

	sel_start = sci_get_selection_start(doc_list[idx].sci);
	sel_end = sci_get_selection_end(doc_list[idx].sci);

	ft = doc_list[idx].file_type;

	first_line = sci_get_line_from_position(doc_list[idx].sci,
		sci_get_selection_start(doc_list[idx].sci));
	/* Find the last line with chars selected (not EOL char) */
	last_line = sci_get_line_from_position(doc_list[idx].sci,
		sci_get_selection_end(doc_list[idx].sci) - utils_get_eol_char_len(idx));
	last_line = MAX(first_line, last_line);

	/* detection of HTML vs PHP code, if non-PHP set filetype to XML */
	first_line_start = sci_get_position_from_line(doc_list[idx].sci, first_line);
	if (ft->id == GEANY_FILETYPES_PHP)
	{
		if (sci_get_style_at(doc_list[idx].sci, first_line_start) < 118 ||
			sci_get_style_at(doc_list[idx].sci, first_line_start) > 127)
			ft = filetypes[GEANY_FILETYPES_XML];
	}

	co = ft->comment_open;
	cc = ft->comment_close;
	if (co == NULL)
		return;

	co_len = strlen(co);
	if (co_len == 0)
		return;

	SSM(doc_list[idx].sci, SCI_BEGINUNDOACTION, 0, 0);

	for (i = first_line; (i <= last_line) && (! break_loop); i++)
	{
		gint buf_len;

		line_start = sci_get_position_from_line(doc_list[idx].sci, i);
		line_len = sci_get_line_length(doc_list[idx].sci, i);
		x = 0;

		buf_len = MIN((gint)sizeof(sel) - 1, line_len - 1);
		if (buf_len < 0)
			continue;
		sci_get_text_range(doc_list[idx].sci, line_start, line_start + buf_len, sel);
		sel[buf_len] = '\0';

		while (isspace(sel[x])) x++;

		/* use single line comment */
		if (cc == NULL || strlen(cc) == 0)
		{
			gboolean do_continue = FALSE;
			single_line = TRUE;

			if (strncmp(sel + x, co, co_len) == 0 &&
				strncmp(sel + x + co_len, GEANY_TOGGLE_MARK, tm_len) == 0)
			{
				do_continue = TRUE;
			}

			if (do_continue && i == first_line)
				first_line_was_comment = TRUE;

			if (do_continue)
			{
				count_uncommented += editor_do_uncomment(idx, i, TRUE);
				continue;
			}

			/* we are still here, so the above lines were not already comments, so comment it */
			editor_do_comment(idx, i, TRUE, TRUE);
			count_commented++;
		}
		/* use multi line comment */
		else
		{
			gint style_comment;
			gint lexer = SSM(doc_list[idx].sci, SCI_GETLEXER, 0, 0);

			/* skip lines which are already comments */
			switch (lexer)
			{	/* I will list only those lexers which support multi line comments */
				case SCLEX_XML:
				case SCLEX_HTML:
				{
					if (sci_get_style_at(doc_list[idx].sci, line_start) >= 118 &&
						sci_get_style_at(doc_list[idx].sci, line_start) <= 127)
						style_comment = SCE_HPHP_COMMENT;
					else style_comment = SCE_H_COMMENT;
					break;
				}
				case SCLEX_CSS: style_comment = SCE_CSS_COMMENT; break;
				case SCLEX_SQL: style_comment = SCE_SQL_COMMENT; break;
				case SCLEX_CAML: style_comment = SCE_CAML_COMMENT; break;
				case SCLEX_D: style_comment = SCE_D_COMMENT; break;
				case SCLEX_RUBY: style_comment = SCE_RB_POD; break;
				case SCLEX_PERL: style_comment = SCE_PL_POD; break;
				default: style_comment = SCE_C_COMMENT;
			}
			if (sci_get_style_at(doc_list[idx].sci, line_start + x) == style_comment)
			{
				real_uncomment_multiline(idx);
				count_uncommented++;
			}
			else
			{
				real_comment_multiline(idx, line_start, last_line);
				count_commented++;
			}

			/* break because we are already on the last line */
			break_loop = TRUE;
			break;
		}
	}

	SSM(doc_list[idx].sci, SCI_ENDUNDOACTION, 0, 0);

	co_len += tm_len;

	/* restore selection if there is one */
	if (sel_start < sel_end)
	{
		if (single_line)
		{
			gint a = (first_line_was_comment) ? - co_len : co_len;

			/* don't modify sel_start when the selection starts within indentation */
			get_indent(&doc_list[idx], sel_start, TRUE);
			if ((sel_start - first_line_start) <= (gint) strlen(indent))
				a = 0;

			sci_set_selection_start(doc_list[idx].sci, sel_start + a);
			sci_set_selection_end(doc_list[idx].sci, sel_end +
								(count_commented * co_len) - (count_uncommented * co_len));
		}
		else
		{
			gint eol_len = utils_get_eol_char_len(idx);
			if (count_uncommented > 0)
			{
				sci_set_selection_start(doc_list[idx].sci, sel_start - co_len - eol_len);
				sci_set_selection_end(doc_list[idx].sci, sel_end - co_len - eol_len);
			}
			else
			{
				sci_set_selection_start(doc_list[idx].sci, sel_start + co_len + eol_len);
				sci_set_selection_end(doc_list[idx].sci, sel_end + co_len + eol_len);
			}
		}
	}
	else if (count_uncommented > 0)
	{
		sci_set_current_position(doc_list[idx].sci, sel_start - co_len, TRUE);
	}
}


/* set toggle to TRUE if the caller is the toggle function, FALSE otherwise */
void editor_do_comment(gint idx, gint line, gboolean allow_empty_lines, gboolean toggle)
{
	gint first_line, last_line;
	gint x, i, line_start, line_len;
	gint sel_start, sel_end, co_len;
	gchar sel[256], *co, *cc;
	gboolean break_loop = FALSE, single_line = FALSE;
	filetype *ft;

	if (! DOC_IDX_VALID(idx) || doc_list[idx].file_type == NULL) return;

	if (line < 0)
	{	/* use selection or current line */
		sel_start = sci_get_selection_start(doc_list[idx].sci);
		sel_end = sci_get_selection_end(doc_list[idx].sci);

		first_line = sci_get_line_from_position(doc_list[idx].sci, sel_start);
		/* Find the last line with chars selected (not EOL char) */
		last_line = sci_get_line_from_position(doc_list[idx].sci,
			sel_end - utils_get_eol_char_len(idx));
		last_line = MAX(first_line, last_line);
	}
	else
	{
		first_line = last_line = line;
		sel_start = sel_end = sci_get_position_from_line(doc_list[idx].sci, line);
	}

	ft = doc_list[idx].file_type;

	/* detection of HTML vs PHP code, if non-PHP set filetype to XML */
	line_start = sci_get_position_from_line(doc_list[idx].sci, first_line);
	if (ft->id == GEANY_FILETYPES_PHP)
	{
		if (sci_get_style_at(doc_list[idx].sci, line_start) < 118 ||
			sci_get_style_at(doc_list[idx].sci, line_start) > 127)
			ft = filetypes[GEANY_FILETYPES_XML];
	}

	co = ft->comment_open;
	cc = ft->comment_close;
	if (co == NULL)
		return;

	co_len = strlen(co);
	if (co_len == 0)
		return;

	SSM(doc_list[idx].sci, SCI_BEGINUNDOACTION, 0, 0);

	for (i = first_line; (i <= last_line) && (! break_loop); i++)
	{
		gint buf_len;

		line_start = sci_get_position_from_line(doc_list[idx].sci, i);
		line_len = sci_get_line_length(doc_list[idx].sci, i);
		x = 0;

		buf_len = MIN((gint)sizeof(sel) - 1, line_len - 1);
		if (buf_len < 0)
			continue;
		sci_get_text_range(doc_list[idx].sci, line_start, line_start + buf_len, sel);
		sel[buf_len] = '\0';

		while (isspace(sel[x])) x++;

		/* to skip blank lines */
		if (allow_empty_lines || (x < line_len && sel[x] != '\0'))
		{
			/* use single line comment */
			if (cc == NULL || strlen(cc) == 0)
			{
				gint start = line_start;
				single_line = TRUE;

				if (ft->comment_use_indent)
					start = line_start + x;

				if (toggle)
				{
					gchar *text = g_strconcat(co, GEANY_TOGGLE_MARK, NULL);
					sci_insert_text(doc_list[idx].sci, start, text);
					g_free(text);
				}
				else
					sci_insert_text(doc_list[idx].sci, start, co);
			}
			/* use multi line comment */
			else
			{
				gint style_comment;
				gint lexer = SSM(doc_list[idx].sci, SCI_GETLEXER, 0, 0);

				/* skip lines which are already comments */
				switch (lexer)
				{	/* I will list only those lexers which support multi line comments */
					case SCLEX_XML:
					case SCLEX_HTML:
					{
						if (sci_get_style_at(doc_list[idx].sci, line_start) >= 118 &&
							sci_get_style_at(doc_list[idx].sci, line_start) <= 127)
							style_comment = SCE_HPHP_COMMENT;
						else style_comment = SCE_H_COMMENT;
						break;
					}
					case SCLEX_CSS: style_comment = SCE_CSS_COMMENT; break;
					case SCLEX_SQL: style_comment = SCE_SQL_COMMENT; break;
					case SCLEX_CAML: style_comment = SCE_CAML_COMMENT; break;
					case SCLEX_D: style_comment = SCE_D_COMMENT; break;
					default: style_comment = SCE_C_COMMENT;
				}
				if (sci_get_style_at(doc_list[idx].sci, line_start + x) == style_comment) continue;

				real_comment_multiline(idx, line_start, last_line);

				/* break because we are already on the last line */
				break_loop = TRUE;
				break;
			}
		}
	}
	SSM(doc_list[idx].sci, SCI_ENDUNDOACTION, 0, 0);

	/* restore selection if there is one
	 * but don't touch the selection if caller is editor_do_comment_toggle */
	if (! toggle && sel_start < sel_end)
	{
		if (single_line)
		{
			sci_set_selection_start(doc_list[idx].sci, sel_start + co_len);
			sci_set_selection_end(doc_list[idx].sci, sel_end + ((i - first_line) * co_len));
		}
		else
		{
			gint eol_len = utils_get_eol_char_len(idx);
			sci_set_selection_start(doc_list[idx].sci, sel_start + co_len + eol_len);
			sci_set_selection_end(doc_list[idx].sci, sel_end + co_len + eol_len);
		}
	}
}


void editor_highlight_braces(ScintillaObject *sci, gint cur_pos)
{
	gint brace_pos = cur_pos - 1;
	gint end_pos;

	if (! utils_isbrace(sci_get_char_at(sci, brace_pos), editor_prefs.brace_match_ltgt))
	{
		brace_pos++;
		if (! utils_isbrace(sci_get_char_at(sci, brace_pos), editor_prefs.brace_match_ltgt))
		{
			SSM(sci, SCI_BRACEBADLIGHT, (uptr_t)-1, 0);
			return;
		}
	}
	end_pos = SSM(sci, SCI_BRACEMATCH, brace_pos, 0);

	if (end_pos >= 0)
		SSM(sci, SCI_BRACEHIGHLIGHT, brace_pos, end_pos);
	else
		SSM(sci, SCI_BRACEBADLIGHT, brace_pos, 0);
}


static gboolean is_doc_comment_char(gchar c, gint lexer)
{
	if (c == '*' && (lexer == SCLEX_HTML || lexer == SCLEX_CPP))
		return TRUE;
	else if ((c == '*' || c == '+') && lexer == SCLEX_D)
		return TRUE;
	else
		return FALSE;
}


static void auto_multiline(gint idx, gint pos)
{
	ScintillaObject *sci = doc_list[idx].sci;
	gint style = SSM(sci, SCI_GETSTYLEAT, pos - 1 - utils_get_eol_char_len(idx), 0);
	gint lexer = SSM(sci, SCI_GETLEXER, 0, 0);
	gint i;

	if ((lexer == SCLEX_CPP && (style == SCE_C_COMMENT || style == SCE_C_COMMENTDOC)) ||
		(lexer == SCLEX_HTML && style == SCE_HPHP_COMMENT) ||
		(lexer == SCLEX_D && (style == SCE_D_COMMENT ||
							  style == SCE_D_COMMENTDOC ||
							  style == SCE_D_COMMENTNESTED)))
	{
		gchar *previous_line = sci_get_line(sci, sci_get_line_from_position(sci, pos - 2));
		/* the type of comment, '*' (C/C++/Java), '+' and the others (D) */
		gchar *continuation = "*";
		gchar *whitespace = ""; /* to hold whitespace if needed */
		gchar *result;
		gint len = strlen(previous_line);

		/* find and stop at end of multi line comment */
		i = len - 1;
		while (i >= 0 && isspace(previous_line[i])) i--;
		if (i >= 1 && is_doc_comment_char(previous_line[i - 1], lexer) && previous_line[i] == '/')
		{
			gint cur_line = sci_get_current_line(sci);
			gint indent_pos = sci_get_line_indent_position(sci, cur_line);
			gint indent_len = sci_get_col_from_position(sci, indent_pos);

			/* if there is one too many spaces, delete the last space,
			 * to return to the indent used before the multiline comment was started. */
			if (indent_len % editor_prefs.tab_width == 1)
				SSM(sci, SCI_DELETEBACKNOTLINE, 0, 0);	/* remove whitespace indent */
			g_free(previous_line);
			return;
		}
		/* check whether we are on the second line of multi line comment */
		i = 0;
		while (i < len && isspace(previous_line[i])) i++; /* get to start of the line */

		if (i + 1 < len &&
			previous_line[i] == '/' && is_doc_comment_char(previous_line[i + 1], lexer))
		{ /* we are on the second line of a multi line comment, so we have to insert white space */
			whitespace = " ";
		}

		if (style == SCE_D_COMMENTNESTED) continuation = "+"; /* for nested comments in D */

		result = g_strconcat(whitespace, continuation, " ", NULL);
		sci_add_text(sci, result);
		g_free(result);

		g_free(previous_line);
	}
}


/* Checks whether the given style is a comment or string for the given lexer.
 * It doesn't handle LEX_HTML, this should be done by the caller.
 * Returns true if the style is a comment, FALSE otherwise.
 */
static gboolean is_comment(gint lexer, gint style)
{
	gboolean result = FALSE;

	switch (lexer)
	{
		case SCLEX_CPP:
		case SCLEX_PASCAL:
		{
			if (style == SCE_C_COMMENT ||
				style == SCE_C_COMMENTLINE ||
				style == SCE_C_COMMENTDOC ||
				style == SCE_C_COMMENTLINEDOC ||
				style == SCE_C_CHARACTER ||
				style == SCE_C_PREPROCESSOR ||
				style == SCE_C_STRING)
				result = TRUE;
			break;
		}
		case SCLEX_D:
		{
			if (style == SCE_D_COMMENT ||
				style == SCE_D_COMMENTLINE ||
				style == SCE_D_COMMENTDOC ||
				style == SCE_D_COMMENTLINEDOC ||
				style == SCE_D_COMMENTNESTED ||
				style == SCE_D_CHARACTER ||
				style == SCE_D_STRING)
				result = TRUE;
			break;
		}
		case SCLEX_PYTHON:
		{
			if (style == SCE_P_COMMENTLINE ||
				style == SCE_P_COMMENTBLOCK ||
				style == SCE_P_STRING)
				result = TRUE;
			break;
		}
		case SCLEX_F77:
		{
			if (style == SCE_F_COMMENT ||
				style == SCE_F_STRING1 ||
				style == SCE_F_STRING2)
				result = TRUE;
			break;
		}
		case SCLEX_PERL:
		{
			if (style == SCE_PL_COMMENTLINE ||
				style == SCE_PL_STRING)
				result = TRUE;
			break;
		}
		case SCLEX_PROPERTIES:
		{
			if (style == SCE_PROPS_COMMENT)
				result = TRUE;
			break;
		}
		case SCLEX_LATEX:
		{
			if (style == SCE_L_COMMENT)
				result = TRUE;
			break;
		}
		case SCLEX_MAKEFILE:
		{
			if (style == SCE_MAKE_COMMENT)
				result = TRUE;
			break;
		}
		case SCLEX_RUBY:
		{
			if (style == SCE_RB_COMMENTLINE ||
				style == SCE_RB_STRING)
				result = TRUE;
			break;
		}
		case SCLEX_BASH:
		{
			if (style == SCE_SH_COMMENTLINE ||
				style == SCE_SH_STRING)
				result = TRUE;
			break;
		}
		case SCLEX_SQL:
		{
			if (style == SCE_SQL_COMMENT ||
				style == SCE_SQL_COMMENTLINE ||
				style == SCE_SQL_COMMENTDOC ||
				style == SCE_SQL_STRING)
				result = TRUE;
			break;
		}
		case SCLEX_TCL:
		{
			if (style == SCE_TCL_COMMENT ||
				style == SCE_TCL_COMMENTLINE ||
				style == SCE_TCL_IN_QUOTE)
				result = TRUE;
			break;
		}
		case SCLEX_LUA:
		{
			if (style == SCE_LUA_COMMENT ||
				style == SCE_LUA_COMMENTLINE ||
				style == SCE_LUA_COMMENTDOC ||
				style == SCE_LUA_LITERALSTRING ||
				style == SCE_LUA_CHARACTER ||
				style == SCE_LUA_STRING)
				result = TRUE;
			break;
		}
		case SCLEX_HASKELL:
		{
			if (style == SCE_HA_COMMENTLINE ||
				style == SCE_HA_COMMENTBLOCK ||
				style == SCE_HA_COMMENTBLOCK2 ||
				style == SCE_HA_COMMENTBLOCK3 ||
				style == SCE_HA_CHARACTER ||
				style == SCE_HA_STRING)
				result = TRUE;
			break;
		}
		case SCLEX_FREEBASIC:
		{
			if (style == SCE_B_COMMENT ||
				style == SCE_B_STRING)
				result = TRUE;
			break;
		}
		case SCLEX_HTML:
		{
			if (style == SCE_HPHP_SIMPLESTRING ||
				style == SCE_HPHP_HSTRING ||
				style == SCE_HPHP_COMMENTLINE ||
				style == SCE_HPHP_COMMENT ||
				style == SCE_H_DOUBLESTRING ||
				style == SCE_H_SINGLESTRING ||
				style == SCE_H_CDATA ||
				style == SCE_H_COMMENT ||
				style == SCE_H_SGML_DOUBLESTRING ||
				style == SCE_H_SGML_SIMPLESTRING ||
				style == SCE_H_SGML_COMMENT)
				result = TRUE;
			break;
		}
	}

	return result;
}


#if 0
gboolean editor_lexer_is_c_like(gint lexer)
{
	switch (lexer)
	{
		case SCLEX_CPP:
		case SCLEX_D:
		return TRUE;

		default:
		return FALSE;
	}
}
#endif


/* Returns: -1 if lexer doesn't support type keywords */
gint editor_lexer_get_type_keyword_idx(gint lexer)
{
	switch (lexer)
	{
		case SCLEX_CPP:
		case SCLEX_D:
		return 3;

		default:
		return -1;
	}
}


/* inserts a three-line comment at one line above current cursor position */
void editor_insert_multiline_comment(gint idx)
{
	gchar *text;
	gint text_len;
	gint line;
	gint pos;
	gboolean have_multiline_comment = FALSE;

	if (doc_list[idx].file_type == NULL || doc_list[idx].file_type->comment_open == NULL)
		return;

	if (doc_list[idx].file_type->comment_close != NULL &&
		strlen(doc_list[idx].file_type->comment_close) > 0)
		have_multiline_comment = TRUE;

	/* insert three lines one line above of the current position */
	line = sci_get_line_from_position(doc_list[idx].sci, editor_info.click_pos);
	pos = sci_get_position_from_line(doc_list[idx].sci, line);

	/* use the indent on the current line but only when comment indentation is used
	 * and we don't have multi line comment characters */
	if (doc_list[idx].auto_indent && ! have_multiline_comment &&
		doc_list[idx].file_type->comment_use_indent)
	{
		get_indent(&doc_list[idx], editor_info.click_pos, TRUE);
		text = g_strdup_printf("%s\n%s\n%s\n", indent, indent, indent);
		text_len = strlen(text);
	}
	else
	{
		text = g_strdup("\n\n\n");
		text_len = 3;
	}
	sci_insert_text(doc_list[idx].sci, pos, text);
	g_free(text);


	/* select the inserted lines for commenting */
	sci_set_selection_start(doc_list[idx].sci, pos);
	sci_set_selection_end(doc_list[idx].sci, pos + text_len);

	editor_do_comment(idx, -1, TRUE, FALSE);

	/* set the current position to the start of the first inserted line */
	pos += strlen(doc_list[idx].file_type->comment_open);

	/* on multi line comment jump to the next line, otherwise add the length of added indentation */
	if (have_multiline_comment)
		pos += 1;
	else
		pos += strlen(indent);

	sci_set_current_position(doc_list[idx].sci, pos, TRUE);
	/* reset the selection */
	sci_set_anchor(doc_list[idx].sci, pos);
}


/* Note: If the editor is pending a redraw, set document::scroll_percent instead.
 * Scroll the view to make line appear at percent_of_view.
 * line can be -1 to use the current position. */
void editor_scroll_to_line(ScintillaObject *sci, gint line, gfloat percent_of_view)
{
	gint vis1, los, delta;
	GtkWidget *wid = GTK_WIDGET(sci);

	if (! wid->window || ! gdk_window_is_viewable(wid->window))
		return;	/* prevent gdk_window_scroll warning */

	if (line == -1)
		line = sci_get_current_line(sci);

	/* sci 'visible line' != doc line number because of folding and line wrapping */
	/* calling SCI_VISIBLEFROMDOCLINE for line is more accurate than calling
	 * SCI_DOCLINEFROMVISIBLE for vis1. */
	line = SSM(sci, SCI_VISIBLEFROMDOCLINE, line, 0);
	vis1 = SSM(sci, SCI_GETFIRSTVISIBLELINE, 0, 0);
	los = SSM(sci, SCI_LINESONSCREEN, 0, 0);
	delta = (line - vis1) - los * percent_of_view;
	sci_scroll_lines(sci, delta);
	sci_scroll_caret(sci); /* needed for horizontal scrolling */
}


void editor_insert_alternative_whitespace(gint idx)
{
	/* creates and inserts one tabulator sign or whitespace of the amount of the tab width */
	gchar *text = get_whitespace(editor_prefs.tab_width, ! doc_list[idx].use_tabs);
	sci_add_text(doc_list[idx].sci, text);
	g_free(text);
}


void editor_select_word(ScintillaObject *sci)
{
	gint pos;
	gint start;
	gint end;

	g_return_if_fail(sci != NULL);

	pos = SSM(sci, SCI_GETCURRENTPOS, 0, 0);
	start = SSM(sci, SCI_WORDSTARTPOSITION, pos, TRUE);
	end = SSM(sci, SCI_WORDENDPOSITION, pos, TRUE);

	if (start == end) /* caret in whitespaces sequence */
	{
		/* look forward but reverse the selection direction,
		 * so the caret end up stay as near as the original position. */
		end = SSM(sci, SCI_WORDENDPOSITION, pos, FALSE);
		start = SSM(sci, SCI_WORDENDPOSITION, end, TRUE);
		if (start == end)
			return;
	}

	SSM(sci, SCI_SETSEL, start, end);
}


/* extra_line is for selecting the cursor line or anchor line at the bottom of a selection,
 * when those lines have no selection. */
void editor_select_lines(ScintillaObject *sci, gboolean extra_line)
{
	gint start, end, line;

	g_return_if_fail(sci != NULL);

	start = sci_get_selection_start(sci);
	end = sci_get_selection_end(sci);

	/* check if whole lines are already selected */
	if (! extra_line && start != end &&
		sci_get_col_from_position(sci, start) == 0 &&
		sci_get_col_from_position(sci, end) == 0)
			return;

	line = sci_get_line_from_position(sci, start);
	start = sci_get_position_from_line(sci, line);

	line = sci_get_line_from_position(sci, end);
	end = sci_get_position_from_line(sci, line + 1);

	SSM(sci, SCI_SETSEL, start, end);
}


/* find the start or end of a paragraph by searching all lines in direction (UP or DOWN)
 * starting at the given line and return the found line or return -1 if called on an empty line */
static gint find_paragraph_stop(ScintillaObject *sci, gint line, gint direction)
{
	gboolean found_end = FALSE;
	gint step;
	gchar *line_buf, *x;

	/* first check current line and return -1 if it is empty to skip creating of a selection */
	line_buf = x = sci_get_line(sci, line);
	while (isspace(*x))
		x++;
	if (*x == '\0')
	{
		g_free(line_buf);
		return -1;
	}

	if (direction == UP)
		step = -1;
	else
		step = 1;

	while (! found_end)
	{
		line += step;

		/* sci_get_line checks for sanity of the given line, sci_get_line always return a string
		 * containing at least '\0' so no need to check for NULL */
		line_buf = x = sci_get_line(sci, line);

		/* check whether after skipping all whitespace we are at end of line and if so, assume
		 * this line as end of paragraph */
		while (isspace(*x))
			x++;
		if (*x == '\0')
		{
			found_end = TRUE;
			if (line == -1)
				/* called on the first line but there is no previous line so return line 0 */
				line = 0;
		}
		g_free(line_buf);
	}
	return line;
}


void editor_select_paragraph(ScintillaObject *sci)
{
	gint pos_start, pos_end, line_start, line_found;

	g_return_if_fail(sci != NULL);

	line_start = SSM(sci, SCI_LINEFROMPOSITION, SSM(sci, SCI_GETCURRENTPOS, 0, 0), 0);

	line_found = find_paragraph_stop(sci, line_start, UP);
	if (line_found == -1)
		return;

	/* find_paragraph_stop returns the emtpy line(previous to the real start of the paragraph),
	 * so use the next line for selection start */
	if (line_found > 0)
		line_found++;

	pos_start = SSM(sci, SCI_POSITIONFROMLINE, line_found, 0);

	line_found = find_paragraph_stop(sci, line_start, DOWN);
	pos_end = SSM(sci, SCI_POSITIONFROMLINE, line_found, 0);

	SSM(sci, SCI_SETSEL, pos_start, pos_end);
}


/* simple auto indentation to indent the current line with the same indent as the previous one */
void editor_auto_line_indentation(gint idx, gint pos)
{
	gint i, first_line, last_line;
	gint first_sel_start, first_sel_end, sel_start = 0, sel_end = 0;

	g_return_if_fail(DOC_IDX_VALID(idx));

	first_sel_start = sci_get_selection_start(doc_list[idx].sci);
	first_sel_end = sci_get_selection_end(doc_list[idx].sci);

	first_line = sci_get_line_from_position(doc_list[idx].sci, first_sel_start);
	/* Find the last line with chars selected (not EOL char) */
	last_line = sci_get_line_from_position(doc_list[idx].sci,
		first_sel_end - utils_get_eol_char_len(idx));
	last_line = MAX(first_line, last_line);

	if (pos == -1)
		pos = first_sel_start;

	/* get previous line and use it for get_indent to use that line
	 * (otherwise it would fail on a line only containing "{" in advanced indentation mode) */
	get_indent(&doc_list[idx],
		sci_get_position_from_line(doc_list[idx].sci, first_line - 1), TRUE);
	SSM(doc_list[idx].sci, SCI_BEGINUNDOACTION, 0, 0);

	for (i = first_line; i <= last_line; i++)
	{
		/* skip the first line or if the indentation of the previous and current line are equal */
		if (i == 0 ||
			SSM(doc_list[idx].sci, SCI_GETLINEINDENTATION, i - 1, 0) ==
			SSM(doc_list[idx].sci, SCI_GETLINEINDENTATION, i, 0))
			continue;

		sel_start = SSM(doc_list[idx].sci, SCI_POSITIONFROMLINE, i, 0);
		sel_end = SSM(doc_list[idx].sci, SCI_GETLINEINDENTPOSITION, i, 0);
		if (sel_start < sel_end)
		{
			SSM(doc_list[idx].sci, SCI_SETSEL, sel_start, sel_end);
			sci_replace_sel(doc_list[idx].sci, "");
		}
		sci_insert_text(doc_list[idx].sci, sel_start, indent);
	}

	/* set cursor position if there was no selection */
	/** TODO implement selection handling if there was a selection */
	if (first_sel_start == first_sel_end)
		sci_set_current_position(doc_list[idx].sci,
			pos - (sel_end - sel_start) + strlen(indent), FALSE);

	SSM(doc_list[idx].sci, SCI_ENDUNDOACTION, 0, 0);
}


/* increase / decrease current line or selection by one space */
void editor_indentation_by_one_space(gint idx, gint pos, gboolean decrease)
{
	gint i, first_line, last_line, line_start, indentation_end, count = 0;
	gint sel_start, sel_end, first_line_offset = 0;

	g_return_if_fail(DOC_IDX_VALID(idx));

	sel_start = sci_get_selection_start(doc_list[idx].sci);
	sel_end = sci_get_selection_end(doc_list[idx].sci);

	first_line = sci_get_line_from_position(doc_list[idx].sci, sel_start);
	/* Find the last line with chars selected (not EOL char) */
	last_line = sci_get_line_from_position(doc_list[idx].sci,
		sel_end - utils_get_eol_char_len(idx));
	last_line = MAX(first_line, last_line);

	if (pos == -1)
		pos = sel_start;

	SSM(doc_list[idx].sci, SCI_BEGINUNDOACTION, 0, 0);

	for (i = first_line; i <= last_line; i++)
	{
		indentation_end = SSM(doc_list[idx].sci, SCI_GETLINEINDENTPOSITION, i, 0);
		if (decrease)
		{
			line_start = SSM(doc_list[idx].sci, SCI_POSITIONFROMLINE, i, 0);
			/* searching backwards for a space to remove */
			while (sci_get_char_at(doc_list[idx].sci, indentation_end) != ' ' &&
				   indentation_end > line_start)
				indentation_end--;

			if (sci_get_char_at(doc_list[idx].sci, indentation_end) == ' ')
			{
				SSM(doc_list[idx].sci, SCI_SETSEL, indentation_end, indentation_end + 1);
				sci_replace_sel(doc_list[idx].sci, "");
				count--;
				if (i == first_line)
					first_line_offset = -1;
			}
		}
		else
		{
			sci_insert_text(doc_list[idx].sci, indentation_end, " ");
			count++;
			if (i == first_line)
				first_line_offset = 1;
		}
	}

	/* set cursor position */
	if (sel_start < sel_end)
	{
		gint start = sel_start + first_line_offset;
		if (first_line_offset < 0)
			start = MAX(sel_start + first_line_offset,
						SSM(doc_list[idx].sci, SCI_POSITIONFROMLINE, first_line, 0));

		sci_set_selection_start(doc_list[idx].sci, start);
		sci_set_selection_end(doc_list[idx].sci, sel_end + count);
	}
	else
		sci_set_current_position(doc_list[idx].sci, pos + count, FALSE);

	SSM(doc_list[idx].sci, SCI_ENDUNDOACTION, 0, 0);
}


void editor_finalize()
{
	g_hash_table_destroy(editor_prefs.snippets);

	scintilla_release_resources();
}


/* wordchars: NULL or a string containing characters to match a word.
 * Returns: the current selection or the current word. */
gchar *editor_get_default_selection(gint idx, gboolean use_current_word, const gchar *wordchars)
{
	gchar *s = NULL;

	if (! DOC_IDX_VALID(idx))
		return NULL;

	if (sci_get_lines_selected(doc_list[idx].sci) == 1)
	{
		gint len = sci_get_selected_text_length(doc_list[idx].sci);

		s = g_malloc(len + 1);
		sci_get_selected_text(doc_list[idx].sci, s);
	}
	else if (sci_get_lines_selected(doc_list[idx].sci) == 0 && use_current_word)
	{	/* use the word at current cursor position */
		gchar word[GEANY_MAX_WORD_LENGTH];

		editor_find_current_word(doc_list[idx].sci, -1, word, sizeof(word), wordchars);
		if (word[0] != '\0')
			s = g_strdup(word);
	}
	return s;
}


/* Note: Usually the line should be made visible (not folded) before calling this.
 * Returns: TRUE if line is/will be displayed to the user, or FALSE if it is
 * outside the view. */
gboolean editor_line_in_view(ScintillaObject *sci, gint line)
{
	gint vis1, los;

	line = SSM(sci, SCI_VISIBLEFROMDOCLINE, line, 0);	/* convert to visible line number */
	vis1 = SSM(sci, SCI_GETFIRSTVISIBLELINE, 0, 0);
	los = SSM(sci, SCI_LINESONSCREEN, 0, 0);

	return (line >= vis1 && line < vis1 + los);
}


/* If the current line is outside the current view window, scroll the line
 * so it appears at percent_of_view. */
void editor_display_current_line(gint idx, gfloat percent_of_view)
{
	ScintillaObject *sci = doc_list[idx].sci;
	gint line = sci_get_current_line(sci);

	/* unfold maybe folded results */
	sci_ensure_line_is_visible(doc_list[idx].sci, line);

	/* scroll the line if it's off screen */
	if (! editor_line_in_view(sci, line))
		doc_list[idx].scroll_percent = percent_of_view;
}


