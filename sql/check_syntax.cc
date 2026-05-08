/* Copyright (c) 2026, MariaDB Foundation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335
   USA
*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation                          // gcc: Class implementation
#endif

#ifndef EMBEDDED_LIBRARY

#include "mariadb.h"
#include "sql_class.h"
#include "m_ctype.h"
#include "sql_parse.h"

class PARSER_STATE
{
public:
  LEX_STRING delimiter;
  const char *query;
  uint start_line, end_line;
  char delimiter_buff[5];
  String rest_of_line;

  PARSER_STATE()
  {
    delimiter_buff[0]= ';';
    delimiter_buff[1]= 0;
    delimiter.str= delimiter_buff;
    delimiter.length= 1;
    start_line= end_line= 1;
  }
  void set_delimiter(const char *str, size_t length)
  {
    length= MY_MIN(sizeof(delimiter_buff)-1, length);
    strmake(delimiter.str, str, length);
    delimiter.length= length;
  }
  void free()
  {
    rest_of_line.free();
  }
};

PARSER_STATE syntax_parser;


static bool is_empty_line(String *line)
{
  const char *pos, *end;
  for (pos= line->ptr(), end= pos + line->length() ; pos < end; pos++)
  {
    if (!my_isspace(line->charset(), *pos))
      return 0;
  }
  return 1;
}


/*
  Remove pre-space in string
*/

static void remove_pre_space(String *line)
{
  size_t line_length= line->length();
  char *ptr= (char*) line->ptr();
  char *pos, *end;

  for (pos= ptr, end= pos + line_length ; pos < end; pos++)
  {
    if (!my_isspace(line->charset(), *pos))
    {
      if (pos != ptr)
      {
        size_t length= (size_t) (end - pos);
        bmove(ptr, pos, length);
        line->length(length);
      }
      return;
    }
  }
  line->length(0);                              // Line only had spaces
}


/**
  Read a query from stdin

  Note that query always ends with a newline, even for the last line
  of a file that may not have a newline.

  @return 0 if ok
  @return 1 if end of file
*/

static bool get_query(THD *thd, MYSQL_FILE *file, String *query)
{
  char buffer[1024], line_buffer[1024];
  CHARSET_INFO *charset_info= query->charset();
  String line(line_buffer, sizeof(line_buffer), charset_info);
  bool eof= 0, found_end= 0;

  query->length(0);
  while (!eof && !found_end)
  {
    const char *pos, *end;
    bool in_comment= 0;
    char in_quote= 0;

    line.length(0);
    syntax_parser.start_line= syntax_parser.end_line;
    remove_pre_space(&syntax_parser.rest_of_line);

    for (found_end= 0 ; !found_end ;)
    {
      if (syntax_parser.rest_of_line.length())
      {
        /* Data left from prevous line */
        line.copy(syntax_parser.rest_of_line);
        syntax_parser.rest_of_line.length(0);
      }
      else
      {
        /* Read a full line */
        while (!(eof= !mysql_file_fgets(buffer, sizeof(buffer), file)))
        {
          end= strend(buffer);
          line.append(buffer, (size_t) (end - buffer));
          if (end[-1] == '\n')
            break;
          if (line.length() + query->length() >
              thd->variables.max_allowed_packet)
          {
            my_printf_error(ER_NET_PACKET_TOO_LARGE,
                            "Line length bigger than max_allowed_packet "
                            "(%lld).  Aborting parsing", MYF(ME_FATAL),
                            (longlong) thd->variables.max_allowed_packet);
            return (1);
          }
        }
        syntax_parser.end_line++;
        found_end= eof;
      }

      if (!line.length() || *(line.ptr()+line.length()-1) != '\n')
      {
        /* Add \n as a safe guard for testing of quotes and comments later */
        line.append('\n');
      }
      line.length(line.length()-1);           // Remove end '\n'

      /* Check if complete line */
      for (pos= line.ptr(), end= pos + line.length() ; pos < end; )
      {
        int length;
        if (charset_info->use_mb() &&
            (length= my_ismbchar(charset_info, pos, end)))
        {
          while (length--)
            pos++;
          continue;
        }
        if (in_quote)
        {
          if (pos[0] == '\\' && pos != end-1)
          {
            pos+= 2;
            continue;
          }
          if (pos[0] != in_quote)
            pos++;
          else if (pos[1] == in_quote)
            pos+= 2;
          else
          {
            in_quote= 0;
            pos++;
          }
          continue;
        }
        if (in_comment)
        {
          if (pos[0] == '*' && pos[1] == '/')
          {
            in_comment= 0;
            pos+= 2;
          }
          else
            pos++;
          continue;
        }
        /* No active comment or quote */
        if (pos[0] == '/' && pos[1] == '*')
        {
          /* executable comment */
          if (pos + 2 < end && pos[2] == '!')
          {
            pos+= 3;
            continue;
          }
          if (pos + 3 < end && pos[2] == 'M' && pos[3] == '!')
          {
            pos+= 4;
            continue;
          }
          in_comment= 1;
          pos+= 2;
          continue;
        }
        if (pos[0] == '#' ||
            ((pos[0] == '-' && pos[1] == '-') &&
             (pos == line.ptr() || my_isspace(charset_info, pos[2]))))
        {
          /* Found end of line comment. Remove comment */
          line.length(pos - line.ptr());
          break;
        }
        if (*pos == '\'' || *pos == '"' || *pos == '`')
        {
          in_quote= *pos++;
          continue;
        }
        if (*pos == '\\' && pos < end-1)
        {
          pos+=2;                               // Escaped character
          continue;
        }

        if (!query->length())
        {
        /* Check if DELIMITER xxx */
          if (!query->charset()->strnncoll(line.ptr(), line.length(),
                                         STRING_WITH_LEN("DELIMITER "), true))
          {
            const char *start= strrchr(line.c_ptr()+9,' ');
            if (start + 1 < line.ptr() + line.length())
            {
              syntax_parser.set_delimiter(start+1,
                                          line.length() -
                                          (start+ 1 - line.ptr()));
              line.length(0);
              break;
            }
          }
        }
        if (!strncmp(pos, syntax_parser.delimiter.str,
                     syntax_parser.delimiter.length))
        {
          size_t length= (size_t) (pos - line.ptr());
          found_end= 1;
          /* Remember rest of line */
          syntax_parser.rest_of_line.copy(pos + syntax_parser.delimiter.length,
                                          line.length() - length -
                                          syntax_parser.delimiter.length,
                                          line.charset());
          line.length(length);        // Remove delimiter
          break;
        }
        pos++;
      }
      if (!in_comment && !in_quote && !found_end &&
          ((line.ptr()[0] == '#' ||
            (line.ptr()[0] == '-' && line.ptr()[1] == '-') ||
            !line.length())))
      {
        if (!query->length())
          syntax_parser.start_line= syntax_parser.end_line;
        continue;                               // Skip empty or comment lines
      }

      query->append(line);

      if (eof && !query->length())
        return 1;
      query->append('\n');
      line.length(0);

#ifdef NOT_TO_BE_USED_BY_DEFAULT
      /*
        If line ends with delimiter + newline then return the line
        if we are in a comment or in a quote. This is to detect
        lines with bad comments or bad quoting
      */
      if (!found_end && !eof && (in_comment || in_quote))
      {
        const char *end= query->ptr() + query->length();
        if (query->length() > syntax_parser.delimiter.length &&
            !strncmp(end - syntax_parser.delimiter.length -1,
                     syntax_parser.delimiter.str,
                     syntax_parser.delimiter.length))
          found_end= 1;
        break;
      }
#endif
    }
    if (is_empty_line(query) && !eof)           // Skipp empty lines
    {
      query->length(0);
      found_end= 0;
      continue;
    }
  }
  /*
    Ensure we do not return empty lines. Note that all found lines
    will end with a newline.
  */
  DBUG_ASSERT(query->length() > 1 || eof);
  return eof && query->length() == 0;
}


/* Ignore all errors */

class plugin_parser_error_handler : public Internal_error_handler
{
public:
  bool handle_condition(THD *thd,
                        uint sql_errno,
                        const char* sqlstate,
                        Sql_condition::enum_warning_level *level,
                        const char* msg,
                        Sql_condition ** cond_hdl) override
  {
    DBUG_ASSERT(strlen(syntax_parser.query) != 0);
    fprintf(stdout, "Line: %6u  Query: '%.*s'  Error '%s' (%d)\n",
            syntax_parser.start_line,
            MY_MIN((int) strlen(syntax_parser.query)-1, // No newline
                   1024),
            syntax_parser.query,
            msg, sql_errno);
    /* Mark error for mysql_parse */
    thd->get_stmt_da()->set_error_status(sql_errno);
    return false;
  }
};


/* This is needed as we have THD on the stack */
PRAGMA_DISABLE_CHECK_STACK_FRAME

bool syntax_checker(MYSQL_FILE *file)
{
  String query;
  bool error= 0;
  THD thd(next_thread_id());
  plugin_parser_error_handler err_handler;
  const LEX_CSTRING dummy_db= { STRING_WITH_LEN("test") };
  DBUG_ENTER("execute_queries");

  thd.store_globals();                    // Setup current_thd and mysys_var
  thd.init();                             // Needed for error messages
  thd.set_db(&dummy_db);
  thd.push_internal_handler(&err_handler);
  query.set_charset(thd.variables.character_set_client);

  if (isatty(fileno(file->m_file)))
  {
    fprintf(stderr, "MariaDB syntax checker(interactive mode)\n");
    fprintf(stderr, "Enter SQL statement followed by a semicolon(;)\n\n");
  }

  while (!get_query(&thd, file, &query))
  {
    Parser_state parser_state;

    /* queries should always end with an end \0 to allow easy printing of it */
    query.append('\0'); query.length(query.length()-1);

    thd.set_query_and_id((char*) query.ptr(), query.length(), thd.charset(),
                         next_query_id());
    thd.set_time();
    if (parser_state.init(&thd, thd.query(), query.length()))
    {
      thd.protocol->end_statement();
      fprintf(stderr, "Got fatal error while initialising parser\n");
      break;
    }
    parser_state.parse_only= 1;
    syntax_parser.query= query.ptr();          // For error printing
    mysql_parse(&thd, thd.query(), query.length(), &parser_state);
    error|= thd.is_error();
    thd.clear_error(1);
    delete_explain_query(thd.lex);
    thd.reset_kill_query();  /* Ensure that killed_errmsg is released */
    free_root(thd.mem_root,MYF(MY_KEEP_PREALLOC));
    // thd.lex->restore_set_statement_var();
    thd.get_stmt_da()->reset_diagnostics_area();
  }
  thd.pop_internal_handler();
  query.free();
  syntax_parser.free();
  set_current_thd(0);
  DBUG_RETURN(error);
}

#endif /* EMBEDDED_LIBRARY */
