function (event) {
    var pedal = event.icon;

    // Rotary Dial Drag Controller
    pedal.find('.custom-knob-dial').each(function () {
        var dial = $(this);
        var sym = dial.attr('data-symbol');
        var min = parseFloat(dial.attr('data-min'));
        var max = parseFloat(dial.attr('data-max'));
        var dflt = parseFloat(dial.attr('data-default'));

        function setRot(val) {
            var norm = (val - min) / (max - min);
            var deg = -140 + norm * 280;
            dial.find('.knob-rotor').css('transform', 'rotate(' + deg + 'deg)');
        }

        setRot(dflt);

        dial.on('mousedown touchstart', function (e) {
            e.preventDefault();
            var startY = e.pageY || e.originalEvent.touches[0].pageY;
            var curVal = parseFloat(dial.attr('data-value') || dflt);

            $(document).on('mousemove.fbknob touchmove.fbknob', function (me) {
                var pageY = me.pageY || me.originalEvent.touches[0].pageY;
                var delta = (startY - pageY) * ((max - min) / 160.0);
                var newVal = Math.max(min, Math.min(max, curVal + delta));

                dial.attr('data-value', newVal);
                setRot(newVal);
                event.set_port_value(sym, newVal);
            });

            $(document).one('mouseup touchend', function () {
                $(document).off('.fbknob');
            });
        });
    });

    // Mode Selector Pills
    pedal.find('.mode-pill').on('click', function () {
        var pill = $(this);
        var modeVal = parseFloat(pill.attr('data-mode'));
        pedal.find('.mode-pill').removeClass('active');
        pill.addClass('active');
        event.set_port_value('mode', modeVal);
    });

    // Feedback Trigger Footswitch (Hold / Momentary)
    var triggerBtn = pedal.find('.trigger-footswitch-btn');
    var triggerLed = pedal.find('#feedback-trigger-led');

    triggerBtn.on('mousedown touchstart', function (e) {
        e.preventDefault();
        triggerBtn.addClass('pressed');
        triggerLed.addClass('on');
        event.set_port_value('trigger', 1.0);
    });

    $(document).on('mouseup.fbtrig touchend.fbtrig', function () {
        if (triggerBtn.hasClass('pressed')) {
            triggerBtn.removeClass('pressed');
            triggerLed.removeClass('on');
            event.set_port_value('trigger', 0.0);
        }
    });

    // Handle Incoming Host Events
    if (event.type === 'change') {
        if (event.symbol === 'mode') {
            var m = Math.round(event.value);
            pedal.find('.mode-pill').removeClass('active');
            pedal.find('.mode-pill[data-mode="' + m + '"]').addClass('active');
        } else if (event.symbol === 'trigger') {
            if (event.value > 0.5) {
                triggerBtn.addClass('pressed');
                triggerLed.addClass('on');
            } else {
                triggerBtn.removeClass('pressed');
                triggerLed.removeClass('on');
            }
        } else {
            var d = pedal.find('.custom-knob-dial[data-symbol="' + event.symbol + '"]');
            if (d.length) {
                var min = parseFloat(d.attr('data-min'));
                var max = parseFloat(d.attr('data-max'));
                var norm = (event.value - min) / (max - min);
                var deg = -140 + norm * 280;
                d.find('.knob-rotor').css('transform', 'rotate(' + deg + 'deg)');
                d.attr('data-value', event.value);
            }
        }
    }
}
