/* gtk-frdp-viewer.vala
 *
 * Copyright (C) 2018 Felipe Borges <felipeborges@gnome.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

using Gtk;
using Frdp;

private class GtkRdpViewer.Application: Gtk.Application {
    private Gtk.ApplicationWindow window;
    private Frdp.Display display;

    public Application () {
        application_id = "org.gnome.GtkRdpViewer";
    }

    protected override void activate () {
        if (window != null)
            return;

        window = new Gtk.ApplicationWindow (this);

        display = new Frdp.Display();
        display.open_host ("10.43.12.92", 3389);

        window.add(display);
        window.show_all ();
    }
}

public int main (string[] args) {
	var app = new GtkRdpViewer.Application ();

    var exit_status = app.run (args);

    return exit_status;
}
