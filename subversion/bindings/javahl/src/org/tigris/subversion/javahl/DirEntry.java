/**
 * @copyright
 * ====================================================================
 * Copyright (c) 2003-2007 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 * @endcopyright
 */

package org.tigris.subversion.javahl;

import java.util.Date;

/**
 * A general subversion directory entry. Used for SVNClientInterface.list
 */
public class DirEntry
{
    /**
     * the date of the last change in nanoseconds since 01/01/1970
     */
    private long lastChanged;

    /**
     * the revision number of the last change
     */
    private long lastChangedRevision;

    /**
     * flag if the item has properties managed by subversion
     */
    private boolean hasProps;

    /**
     * the name of the author of the last change
     */
    private String lastAuthor;

    /**
     * the kind of the node (directory or file)
     */
    private int nodeKind;

    /**
     * the size of the file
     */
    private long size;

    /**
     * the pathname of the entry
     */
    private String path;

    /**
     * the absolute path of the entry
     */
    private String absPath;

    /**
     * this constructor is only called from the JNI code
     * @param path                  the pathname of the entry
     * @param absPath               the absolute path of the entry
     * @param nodeKind              the kind of entry (file or directory)
     * @param size                  the size of the file
     * @param hasProps              if the entry has properties managed by
     *                              subversion
     * @param lastChangedRevision   the revision number of the last change
     * @param lastChanged           the date of the last change
     * @param lastAuthor            the author of the last change
     */
    DirEntry(String path, String absPath, int nodeKind, long size,
             boolean hasProps, long lastChangedRevision, long lastChanged,
             String lastAuthor)
    {
        this.path = path;
        this.absPath = absPath;
        this.nodeKind = nodeKind;
        this.size = size;
        this.hasProps = hasProps;
        this.lastChangedRevision = lastChangedRevision;
        this.lastChanged = lastChanged;
        this.lastAuthor = lastAuthor;
    }

    /**
     * Returns the path of the entry.
     * @return the path of the entry.
     */
    public String getPath()
    {
        return path;
    }

    /**
     * Returns the absolute path of the entry.
     * @return the absolute path of the entry.
     */
    public String getAbsPath()
    {
        return absPath;
    }

    /**
     * Returns the last time the file was changed.
     * @return the last time the file was changed.
     */
    public Date getLastChanged()
    {
        return new Date(lastChanged/1000);
    }

    /**
     * Returns the revision of the last change.
     * @return revision of the last change as a Revision object.
     */
    public Revision.Number getLastChangedRevision()
    {
        return Revision.createNumber(lastChangedRevision);
    }

    /**
     * Returns the revision number of the last change.
     * @return revision number of the last change.
     */
    public long getLastChangedRevisionNumber()
    {
        return lastChangedRevision;
    }

    /**
     * Returns if the entry has properties managed by Subversion.
     * @return if the entry has properties managed by subversion.
     */
    public boolean getHasProps()
    {
        return hasProps;
    }

    /**
     * Returns the author of the last change.
     * @return the author of the last change.
     */
    public String getLastAuthor()
    {
        return lastAuthor;
    }

    /**
     * Return the kind of entry (file or directory)
     * @return the kind of the entry (file or directory) see NodeKind class
     */
    public int getNodeKind()
    {
        return nodeKind;
    }

    /**
     * Return the length of file test or 0 for directories
     * @return length of file text, or 0 for directories
     */
    public long getSize()
    {
        return size;
    }

    /**
     * Set the path.  This should only be used by compatibility wrapper.
     */
    public void setPath(String path)
    {
        this.path = path;
    }

    /**
     * @return The path at its last changed revision.
     */
    public String toString()
    {
        return getPath() + '@' + getLastChangedRevision();
    }
}
